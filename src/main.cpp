/**
 * Dictionary Lookup App
 *
 * A GTK3 GUI application that queries the Free Dictionary API
 * (https://api.dictionaryapi.dev/) and displays word definitions,
 * pronunciations, parts of speech, and more.
 */

#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// RAII wrapper for GObject references
struct GObjectUnref {
    void operator()(gpointer obj) const { g_object_unref(obj); }
};
template <typename T>
using GObjectPtr = std::unique_ptr<T, GObjectUnref>;

// Supported languages: display name → API language code
static const std::vector<std::pair<std::string, std::string>> LANGUAGES = {
    {"English",               "en"},
    {"Arabic",                "ar"},
    {"German",                "de"},
    {"Spanish",               "es"},
    {"French",                "fr"},
    {"Hindi",                 "hi"},
    {"Italian",               "it"},
    {"Japanese",              "ja"},
    {"Korean",                "ko"},
    {"Brazilian Portuguese",  "pt-BR"},
    {"Russian",               "ru"},
    {"Turkish",               "tr"},
};

// Widgets and state shared across callbacks
struct AppData {
    GtkWidget    *word_entry;
    GtkWidget    *lang_combo;
    GtkWidget    *lookup_button;
    GtkWidget    *result_view;
    GtkTextBuffer *result_buffer;
    SoupSession  *session;
};

// Payload carried from the lookup dispatch to the async callback
struct LookupPayload {
    AppData               *app;
    GObjectPtr<SoupMessage> msg;   // RAII-managed reference
};

// ── Formatting helpers ────────────────────────────────────────────────────────

static void append_list(std::ostringstream &oss,
                        const json &arr,
                        const std::string &label)
{
    if (!arr.is_array() || arr.empty())
        return;
    oss << "   " << label << ": ";
    bool first = true;
    for (const auto &item : arr) {
        if (item.is_string() && !item.get<std::string>().empty()) {
            if (!first) oss << ", ";
            oss << item.get<std::string>();
            first = false;
        }
    }
    if (!first) oss << "\n";
}

static std::string format_results(const json &entries)
{
    std::ostringstream oss;

    for (const auto &entry : entries) {
        // Word
        std::string word = entry.value("word", "");
        oss << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
        oss << "Word: " << word << "\n";

        // Phonetics – prefer entries that carry actual text
        bool phonetic_printed = false;
        if (entry.contains("phonetics") && entry["phonetics"].is_array()) {
            for (const auto &ph : entry["phonetics"]) {
                std::string text = ph.value("text", "");
                if (!text.empty()) {
                    oss << "Phonetic: " << text << "\n";
                    phonetic_printed = true;
                    break;
                }
            }
        }
        if (!phonetic_printed && entry.contains("phonetic")) {
            std::string ph = entry.value("phonetic", "");
            if (!ph.empty())
                oss << "Phonetic: " << ph << "\n";
        }

        oss << "\n";

        // Meanings
        if (entry.contains("meanings") && entry["meanings"].is_array()) {
            for (const auto &meaning : entry["meanings"]) {
                std::string pos = meaning.value("partOfSpeech", "");
                oss << "── " << pos << " ──\n";

                if (meaning.contains("definitions") &&
                    meaning["definitions"].is_array())
                {
                    int idx = 1;
                    for (const auto &defn : meaning["definitions"]) {
                        std::string def = defn.value("definition", "");
                        oss << idx++ << ". " << def << "\n";

                        std::string ex = defn.value("example", "");
                        if (!ex.empty())
                            oss << "   Example: \"" << ex << "\"\n";

                        append_list(oss, defn.value("synonyms", json::array()),
                                    "Synonyms");
                        append_list(oss, defn.value("antonyms", json::array()),
                                    "Antonyms");
                    }
                }

                append_list(oss, meaning.value("synonyms", json::array()),
                            "Synonyms");
                append_list(oss, meaning.value("antonyms", json::array()),
                            "Antonyms");
                oss << "\n";
            }
        }

        // Source URLs
        if (entry.contains("sourceUrls") &&
            entry["sourceUrls"].is_array() &&
            !entry["sourceUrls"].empty())
        {
            oss << "Source: "
                << entry["sourceUrls"][0].get<std::string>() << "\n";
        }

        oss << "\n";
    }

    return oss.str();
}

// ── Async HTTP callback ───────────────────────────────────────────────────────

static void on_soup_response(GObject *source,
                             GAsyncResult *result,
                             gpointer user_data)
{
    // Reclaim ownership so the payload (and its GObjectPtr<SoupMessage>)
    // are automatically freed when this scope exits.
    std::unique_ptr<LookupPayload> payload(
        static_cast<LookupPayload *>(user_data));
    AppData *app = payload->app;

    GError *error = nullptr;
    GBytes *bytes = soup_session_send_and_read_finish(
                        SOUP_SESSION(source), result, &error);

    // Re-enable the button regardless of outcome
    gtk_widget_set_sensitive(app->lookup_button, TRUE);

    if (error) {
        std::string msg = std::string("Network error: ") + error->message;
        gtk_text_buffer_set_text(app->result_buffer, msg.c_str(), -1);
        g_error_free(error);
        return;
    }

    gsize data_size = 0;
    const gchar *data =
        static_cast<const gchar *>(g_bytes_get_data(bytes, &data_size));
    std::string json_str(data, data_size);
    g_bytes_unref(bytes);

    try {
        json parsed = json::parse(json_str);

        if (parsed.is_array()) {
            std::string formatted = format_results(parsed);
            gtk_text_buffer_set_text(app->result_buffer,
                                     formatted.c_str(), -1);
        } else if (parsed.is_object()) {
            // API error response: { "title", "message", "resolution" }
            std::string title      = parsed.value("title", "Error");
            std::string message    = parsed.value("message", "");
            std::string resolution = parsed.value("resolution", "");
            std::string text       = title;
            if (!message.empty())    text += "\n\n" + message;
            if (!resolution.empty()) text += "\n\n" + resolution;
            gtk_text_buffer_set_text(app->result_buffer, text.c_str(), -1);
        } else {
            gtk_text_buffer_set_text(app->result_buffer,
                                     "Unexpected response from the server.",
                                     -1);
        }
    } catch (const std::exception &e) {
        std::string msg = std::string("Parse error: ") + e.what();
        gtk_text_buffer_set_text(app->result_buffer, msg.c_str(), -1);
    }
}

// ── Lookup dispatch ───────────────────────────────────────────────────────────

static void do_lookup(AppData *app)
{
    const gchar *word_text = gtk_entry_get_text(GTK_ENTRY(app->word_entry));
    if (!word_text || word_text[0] == '\0') {
        gtk_text_buffer_set_text(app->result_buffer,
                                 "Please enter a word to look up.", -1);
        return;
    }

    gint lang_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->lang_combo));
    if (lang_idx < 0 || lang_idx >= static_cast<gint>(LANGUAGES.size()))
        lang_idx = 0;
    const std::string &lang_code = LANGUAGES[static_cast<size_t>(lang_idx)].second;

    gchar *encoded = g_uri_escape_string(word_text, nullptr, FALSE);
    std::string url = "https://api.dictionaryapi.dev/api/v2/entries/"
                    + lang_code + "/" + std::string(encoded);
    g_free(encoded);

    gtk_text_buffer_set_text(app->result_buffer,
                             "Looking up definition…", -1);
    gtk_widget_set_sensitive(app->lookup_button, FALSE);

    SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, url.c_str());
    // Use unique_ptr for safe construction; release() transfers ownership
    // to the C callback (reclaimed via unique_ptr in on_soup_response).
    auto payload = std::make_unique<LookupPayload>(
        LookupPayload{app, GObjectPtr<SoupMessage>(
                               static_cast<SoupMessage *>(g_object_ref(msg)))});

    soup_session_send_and_read_async(app->session, msg,
                                     G_PRIORITY_DEFAULT,
                                     nullptr,
                                     on_soup_response,
                                     payload.release());
    g_object_unref(msg);
}

static void on_lookup_clicked(GtkButton * /*button*/, gpointer user_data)
{
    do_lookup(static_cast<AppData *>(user_data));
}

static void on_word_entry_activate(GtkEntry * /*entry*/, gpointer user_data)
{
    do_lookup(static_cast<AppData *>(user_data));
}

// ── Window construction ───────────────────────────────────────────────────────

static void activate(GtkApplication *gtkapp, gpointer user_data)
{
    auto *app = static_cast<AppData *>(user_data);

    GtkWidget *window = gtk_application_window_new(gtkapp);
    gtk_window_set_title(GTK_WINDOW(window), "Dictionary Lookup");
    gtk_window_set_default_size(GTK_WINDOW(window), 720, 580);
    gtk_container_set_border_width(GTK_CONTAINER(window), 12);

    // Root vertical box
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    // Header
    GtkWidget *header = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(header),
        "<span size='x-large' weight='bold'>📖  Dictionary Lookup</span>");
    gtk_widget_set_halign(header, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    // Input grid: Word / Language / Button
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 6);

    // Row 0: Word
    GtkWidget *word_label = gtk_label_new("Word:");
    gtk_widget_set_halign(word_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), word_label, 0, 0, 1, 1);

    app->word_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->word_entry),
                                   "Enter a word…");
    gtk_widget_set_hexpand(app->word_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->word_entry, 1, 0, 1, 1);
    g_signal_connect(app->word_entry, "activate",
                     G_CALLBACK(on_word_entry_activate), app);

    // Row 1: Language
    GtkWidget *lang_label = gtk_label_new("Language:");
    gtk_widget_set_halign(lang_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lang_label, 0, 1, 1, 1);

    app->lang_combo = gtk_combo_box_text_new();
    for (const auto &lang : LANGUAGES)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->lang_combo),
                                       lang.first.c_str());
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->lang_combo), 0);
    gtk_grid_attach(GTK_GRID(grid), app->lang_combo, 1, 1, 1, 1);

    // Lookup button spans both rows
    app->lookup_button = gtk_button_new_with_label("🔍  Look Up");
    gtk_widget_set_vexpand(app->lookup_button, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->lookup_button, 2, 0, 1, 2);
    g_signal_connect(app->lookup_button, "clicked",
                     G_CALLBACK(on_lookup_clicked), app);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    // Results label
    GtkWidget *results_label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(results_label), "<b>Results</b>");
    gtk_widget_set_halign(results_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), results_label, FALSE, FALSE, 0);

    // Scrollable text view
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    app->result_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->result_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->result_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->result_view),
                                GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->result_view), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->result_view), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(app->result_view), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(app->result_view), 8);
    gtk_container_add(GTK_CONTAINER(scroll), app->result_view);

    app->result_buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->result_view));
    gtk_text_buffer_set_text(
        app->result_buffer,
        "Enter a word above and click \"Look Up\" to see its definition.\n\n"
        "Supported languages: English, Arabic, German, Spanish, French, "
        "Hindi, Italian, Japanese, Korean, Brazilian Portuguese, Russian, "
        "Turkish.",
        -1);

    gtk_widget_show_all(window);
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main(int argc, char **argv)
{
    AppData app{};
    app.session = soup_session_new();

    GtkApplication *gtkapp =
        gtk_application_new("org.dictionary.lookup",
                            G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtkapp, "activate", G_CALLBACK(activate), &app);

    int status = g_application_run(G_APPLICATION(gtkapp), argc, argv);

    g_object_unref(gtkapp);
    g_object_unref(app.session);

    return status;
}
