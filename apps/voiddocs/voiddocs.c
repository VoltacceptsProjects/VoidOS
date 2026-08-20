/* VoidDocs -- the VoidOS Rich Void Text editor
 *
 * A small GTK3 app for VoidOS's native document format, `.rvt`
 * (Rich Void Text). Under the hood it's plain text with a mix of
 * Markdown and extra rich-text markers:
 *
 *   **bold**, *italic*, `code`, [link](url),
 *   __underline__, ~~strikethrough~~, ==highlight==,
 *   ^superscript^, ~subscript~
 *
 * Single combined page: one GtkTextView is both the editor and the
 * live preview. Formatting (bold/italic/headers/links/...) is
 * rendered in place as you type via text tags; a "Show/Hide Markup"
 * toggle controls whether the Rich Void Text punctuation itself
 * (**, __, `, [...](...), etc.) is visible or hidden, so the same
 * page works either as a raw source view or a clean WYSIWYG view.
 * No HTML engine involved -- just a GtkTextView styled with tags,
 * which keeps this dependency-light and fast.
 *
 * The chrome is a Word-style ribbon: a dark-blue quick-access strip,
 * a clickable tab row (File / Home / Insert / Layout / Review /
 * View -- every tab has real, working commands), and the document
 * itself rendered as a white "page" floating on a gray canvas --
 * the classic Office print-layout look.
 *
 * Runs fine standalone under any GTK-capable WM; under voidwm it
 * gets a titlebar + rounded frame for free, and this window's title
 * (kept in sync with the open file) is what voidwm's titlebar shows.
 */
#include <gtk/gtk.h>
#include <string.h>

#define APP_TITLE        "VoidDocs"
#define VDOC_EXTENSION   ".rvt"
#define DEFAULT_FILENAME "untitled.rvt"

/* ---- "Word 2026" Fluent-ish chrome palette ---- */
#define CSS_BG          "#EAEAEA"   /* gray canvas behind the page */
#define CSS_PANEL_BG    "#F3F2F1"   /* ribbon body */
#define CSS_EDGE        "#D6D6D6"   /* hairline borders */
#define CSS_TITLEBAR    "#185ABD"   /* quick-access strip */
#define CSS_TITLEBAR_DK "#0F3E85"
#define CSS_TEXT        "#201F1E"
#define CSS_TEXT_MUTED  "#5B5B5B"
#define CSS_ACCENT      "#185ABD"
#define CSS_ACCENT_HOV  "#2B7CD3"
#define CSS_ACCENT_TINT "#E8F1FC"

/* ---- White page/document palette ---- */
#define PAGE_BG          "#ffffff"
#define PAGE_TEXT        "#1a1a1a"
#define PAGE_MUTED       "#6b5d55"
#define PAGE_CODE_BG     "#f2f2f2"
#define PAGE_CODE_TEXT   "#8a3b2b"
#define PAGE_HIGHLIGHT   "#fff3b0"
#define PAGE_MARKUP_DIM  "#b9b3ad"  /* dimmed color for RVF punctuation */

#define N_TABS 6

typedef struct {
    GtkWidget     *window;
    GtkWidget     *editor_view;
    GtkWidget     *status_label;
    GtkWidget     *titlebar_doc_label;
    GtkWidget     *zoom_label;
    GtkWidget     *ribbon_stack;
    GtkWidget     *tab_buttons[N_TABS];
    GtkWidget     *markup_toggle_btn;       /* Home tab's toggle */
    GtkWidget     *markup_toggle_btn_view;  /* View tab's toggle */
    GtkTextBuffer *buf;
    GtkTextTag    *markup_tag;
    gchar         *filepath;   /* NULL if never saved */
    gboolean       modified;
    gboolean       show_markup;
    gdouble        zoom_level;
    guint          highlight_idle_id;
} AppState;

/* ================================================================
 * Small helpers
 * ================================================================ */

static const char *
display_name(AppState *st)
{
    if (!st->filepath) return DEFAULT_FILENAME;
    const char *slash = strrchr(st->filepath, '/');
    return slash ? slash + 1 : st->filepath;
}

static void
update_title(AppState *st)
{
    gchar *title = g_strdup_printf("%s%s \xe2\x80\x94 " APP_TITLE,
                                    st->modified ? "\xe2\x97\x8f " : "",
                                    display_name(st));
    gtk_window_set_title(GTK_WINDOW(st->window), title);
    g_free(title);

    gchar *status = g_strdup_printf("%s%s",
                                     st->filepath ? st->filepath : "New document (not yet saved)",
                                     st->modified ? "  \xe2\x80\x94  edited" : "");
    gtk_label_set_text(GTK_LABEL(st->status_label), status);
    g_free(status);

    if (st->titlebar_doc_label) {
        gchar *doc = g_strdup_printf("%s%s",
                                      display_name(st),
                                      st->modified ? " \xe2\x97\x8f" : "");
        gtk_label_set_text(GTK_LABEL(st->titlebar_doc_label), doc);
        g_free(doc);
    }
}

static void
set_modified(AppState *st, gboolean m)
{
    if (st->modified == m) return;
    st->modified = m;
    update_title(st);
}

/* ================================================================
 * Tag setup for the single combined editor/preview buffer
 * ================================================================ */

static void
install_tags(GtkTextBuffer *buf, AppState *st)
{
    gtk_text_buffer_create_tag(buf, "bold",   "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(buf, "italic", "style",  PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(buf, "underline", "underline", PANGO_UNDERLINE_SINGLE, NULL);
    gtk_text_buffer_create_tag(buf, "strikethrough", "strikethrough", TRUE, NULL);
    gtk_text_buffer_create_tag(buf, "highlight", "background", PAGE_HIGHLIGHT, NULL);
    gtk_text_buffer_create_tag(buf, "superscript", "rise", 4 * PANGO_SCALE, "scale", 0.8, NULL);
    gtk_text_buffer_create_tag(buf, "subscript", "rise", -3 * PANGO_SCALE, "scale", 0.8, NULL);

    gtk_text_buffer_create_tag(buf, "code",
                                "family", "monospace",
                                "foreground", PAGE_CODE_TEXT,
                                "background", PAGE_CODE_BG, NULL);
    gtk_text_buffer_create_tag(buf, "link",
                                "foreground", CSS_ACCENT_HOV,
                                "underline", PANGO_UNDERLINE_SINGLE, NULL);
    gtk_text_buffer_create_tag(buf, "list-marker", "foreground", CSS_ACCENT, "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(buf, "blockquote-marker", "foreground", CSS_ACCENT, NULL);
    gtk_text_buffer_create_tag(buf, "blockquote",
                                "style", PANGO_STYLE_ITALIC,
                                "foreground", PAGE_MUTED,
                                "left-margin", 16, NULL);
    gtk_text_buffer_create_tag(buf, "hr", "foreground", PAGE_MUTED, NULL);

    /* RVF punctuation itself: dimmed while visible, and this is the
     * tag whose "invisible" property gets flipped by the Show/Hide
     * Markup toggle -- that's what turns this single page into a
     * clean rendered view without needing a second pane/buffer. */
    st->markup_tag = gtk_text_buffer_create_tag(buf, "markup",
                                                 "foreground", PAGE_MARKUP_DIM,
                                                 "scale", 0.94, NULL);

    static const double scales[7] = { 0, 1.9, 1.6, 1.35, 1.18, 1.05, 1.0 };
    for (int lvl = 1; lvl <= 6; lvl++) {
        char name[16];
        g_snprintf(name, sizeof name, "header%d", lvl);
        gtk_text_buffer_create_tag(buf, name,
                                    "weight", PANGO_WEIGHT_BOLD,
                                    "scale", scales[lvl],
                                    "foreground", lvl <= 2 ? CSS_ACCENT : PAGE_TEXT,
                                    "pixels-above-lines", 6,
                                    "pixels-below-lines", 2,
                                    NULL);
    }
}

/* ================================================================
 * Live in-place formatting for the combined page
 * ================================================================ */

static void
apply_range(GtkTextBuffer *buf, int line, int col0, int col1, const char *tag)
{
    if (!tag || col1 <= col0) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line_offset(buf, &a, line, col0);
    gtk_text_buffer_get_iter_at_line_offset(buf, &b, line, col1);
    gtk_text_buffer_apply_tag_by_name(buf, tag, &a, &b);
}

/* Tags one Rich Void Text inline delimiter pair: the delimiter
 * runs get "markup" (dimmed, and hideable), the inner text gets the
 * real formatting tag. */
static void
apply_delim(GtkTextBuffer *buf, int line, int open0, int open1, int close0, int close1, const char *tag)
{
    apply_range(buf, line, open0, open1, "markup");
    apply_range(buf, line, open1, close0, tag);
    apply_range(buf, line, close0, close1, "markup");
}

static void
highlight_line(GtkTextBuffer *buf, int line, const char *text)
{
    int len = (int)strlen(text);
    int lead = 0;
    while (text[lead] == ' ') lead++;
    const char *p = text + lead;

    if (p[0] == '#') {
        int level = 0, j = lead;
        while (text[j] == '#' && level < 6) { level++; j++; }
        int content_start = j;
        if (text[content_start] == ' ') content_start++;
        char tagname[16];
        g_snprintf(tagname, sizeof tagname, "header%d", level);
        apply_range(buf, line, lead, content_start, "markup");
        apply_range(buf, line, content_start, len, tagname);
        return;
    }
    if (p[0] == '>') {
        int j = lead + 1;
        if (text[j] == ' ') j++;
        apply_range(buf, line, lead, j, "blockquote-marker");
        apply_range(buf, line, j, len, "blockquote");
        return;
    }
    if (strcmp(p, "---") == 0 || strcmp(p, "***") == 0) {
        apply_range(buf, line, lead, len, "hr");
        return;
    }

    int i = lead;
    if ((p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ') {
        apply_range(buf, line, lead, lead + 1, "list-marker");
        i = lead + 2;
    } else {
        int j = lead;
        while (g_ascii_isdigit(text[j])) j++;
        if (j > lead && text[j] == '.' && text[j + 1] == ' ') {
            apply_range(buf, line, lead, j + 1, "list-marker");
            i = j + 2;
        }
    }

    while (i < len) {
        if (text[i] == '*' && i + 1 < len && text[i + 1] == '*') {
            int j = i + 2;
            while (j + 1 < len && !(text[j] == '*' && text[j + 1] == '*')) j++;
            if (j + 1 < len) { apply_delim(buf, line, i, i + 2, j, j + 2, "bold"); i = j + 2; continue; }
        }
        if (text[i] == '_' && i + 1 < len && text[i + 1] == '_') {
            int j = i + 2;
            while (j + 1 < len && !(text[j] == '_' && text[j + 1] == '_')) j++;
            if (j + 1 < len) { apply_delim(buf, line, i, i + 2, j, j + 2, "underline"); i = j + 2; continue; }
        }
        if (text[i] == '~' && i + 1 < len && text[i + 1] == '~') {
            int j = i + 2;
            while (j + 1 < len && !(text[j] == '~' && text[j + 1] == '~')) j++;
            if (j + 1 < len) { apply_delim(buf, line, i, i + 2, j, j + 2, "strikethrough"); i = j + 2; continue; }
        }
        if (text[i] == '=' && i + 1 < len && text[i + 1] == '=') {
            int j = i + 2;
            while (j + 1 < len && !(text[j] == '=' && text[j + 1] == '=')) j++;
            if (j + 1 < len) { apply_delim(buf, line, i, i + 2, j, j + 2, "highlight"); i = j + 2; continue; }
        }
        if (text[i] == '`') {
            int j = i + 1;
            while (j < len && text[j] != '`') j++;
            if (j < len) { apply_delim(buf, line, i, i + 1, j, j + 1, "code"); i = j + 1; continue; }
        }
        if (text[i] == '[') {
            int close = i + 1;
            while (close < len && text[close] != ']') close++;
            if (close < len && text[close + 1] == '(') {
                int urlend = close + 2;
                while (urlend < len && text[urlend] != ')') urlend++;
                if (urlend < len) {
                    apply_range(buf, line, i, i + 1, "markup");            /* [ */
                    apply_range(buf, line, i + 1, close, "link");          /* label */
                    apply_range(buf, line, close, urlend + 1, "markup");   /* ](url) */
                    i = urlend + 1; continue;
                }
            }
        }
        if (text[i] == '^') {
            int j = i + 1;
            while (j < len && text[j] != '^') j++;
            if (j < len && j > i + 1) { apply_delim(buf, line, i, i + 1, j, j + 1, "superscript"); i = j + 1; continue; }
        }
        if (text[i] == '~') {
            int j = i + 1;
            while (j < len && text[j] != '~') j++;
            if (j < len && j > i + 1) { apply_delim(buf, line, i, i + 1, j, j + 1, "subscript"); i = j + 1; continue; }
        }
        if (text[i] == '*') {
            int j = i + 1;
            while (j < len && text[j] != '*') j++;
            if (j < len && j > i + 1) { apply_delim(buf, line, i, i + 1, j, j + 1, "italic"); i = j + 1; continue; }
        }
        i++;
    }
}

static void
clear_all_tags(GtkTextBuffer *buf)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(buf, &s, &e);
    static const char *names[] = {
        "bold", "italic", "underline", "strikethrough", "highlight",
        "superscript", "subscript", "code", "link", "list-marker",
        "blockquote-marker", "blockquote", "hr", "markup",
        "header1", "header2", "header3", "header4", "header5", "header6", NULL
    };
    for (int i = 0; names[i]; i++)
        gtk_text_buffer_remove_tag_by_name(buf, names[i], &s, &e);
}

static gboolean
highlight_all_idle(gpointer data)
{
    AppState *st = data;
    st->highlight_idle_id = 0;

    clear_all_tags(st->buf);
    int nlines = gtk_text_buffer_get_line_count(st->buf);
    for (int ln = 0; ln < nlines; ln++) {
        GtkTextIter ls, le;
        gtk_text_buffer_get_iter_at_line(st->buf, &ls, ln);
        le = ls;
        gtk_text_iter_forward_to_line_end(&le);
        gchar *text = gtk_text_buffer_get_text(st->buf, &ls, &le, FALSE);
        highlight_line(st->buf, ln, text);
        g_free(text);
    }
    return G_SOURCE_REMOVE;
}

static void
schedule_highlight(AppState *st)
{
    if (st->highlight_idle_id) return;
    st->highlight_idle_id = g_idle_add(highlight_all_idle, st);
}

static void
on_buffer_changed(GtkTextBuffer *buf, gpointer data)
{
    (void)buf;
    AppState *st = data;
    set_modified(st, TRUE);
    schedule_highlight(st);
}

/* ================================================================
 * File I/O
 * ================================================================ */

static gboolean
confirm_discard(AppState *st)
{
    if (!st->modified) return TRUE;
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(st->window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "\"%s\" has unsaved changes.", display_name(st));
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
        "Discard the changes and continue?");
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
        "Cancel", GTK_RESPONSE_CANCEL,
        "Discard", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CANCEL);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return resp == GTK_RESPONSE_OK;
}

static void
load_file(AppState *st, const gchar *path)
{
    gchar *contents = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(path, &contents, &len, &err)) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(st->window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Couldn't open \"%s\": %s", path, err->message);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_error_free(err);
        return;
    }
    gtk_text_buffer_set_text(st->buf, contents, (gint)len);
    g_free(contents);

    g_free(st->filepath);
    st->filepath = g_strdup(path);
    set_modified(st, FALSE);
    update_title(st);
    schedule_highlight(st);
}

static gchar *
ensure_vdoc_extension(const gchar *path)
{
    if (g_str_has_suffix(path, VDOC_EXTENSION) || strchr(path, '.') != NULL)
        return g_strdup(path);
    return g_strconcat(path, VDOC_EXTENSION, NULL);
}

static gboolean
save_to(AppState *st, const gchar *path)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(st->buf, &s, &e);
    gchar *text = gtk_text_buffer_get_text(st->buf, &s, &e, FALSE);
    GError *err = NULL;
    gboolean ok = g_file_set_contents(path, text, -1, &err);
    g_free(text);
    if (!ok) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(st->window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Couldn't save \"%s\": %s", path, err->message);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_error_free(err);
        return FALSE;
    }
    g_free(st->filepath);
    st->filepath = g_strdup(path);
    set_modified(st, FALSE);
    return TRUE;
}

static void
add_filters(GtkFileChooser *chooser)
{
    GtkFileFilter *rvf = gtk_file_filter_new();
    gtk_file_filter_set_name(rvf, "Rich Void Text (*.rvt)");
    gtk_file_filter_add_pattern(rvf, "*.rvt");
    gtk_file_chooser_add_filter(chooser, rvf);

    GtkFileFilter *vdoc = gtk_file_filter_new();
    gtk_file_filter_set_name(vdoc, "VoidDocs Markdown (*.vdoc)");
    gtk_file_filter_add_pattern(vdoc, "*.vdoc");
    gtk_file_chooser_add_filter(chooser, vdoc);

    GtkFileFilter *md = gtk_file_filter_new();
    gtk_file_filter_set_name(md, "Markdown (*.md, *.markdown)");
    gtk_file_filter_add_pattern(md, "*.md");
    gtk_file_filter_add_pattern(md, "*.markdown");
    gtk_file_chooser_add_filter(chooser, md);

    GtkFileFilter *txt = gtk_file_filter_new();
    gtk_file_filter_set_name(txt, "Text files (*.txt)");
    gtk_file_filter_add_pattern(txt, "*.txt");
    gtk_file_chooser_add_filter(chooser, txt);

    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(chooser, all);

    gtk_file_chooser_set_filter(chooser, rvf);
}

static gboolean action_save(AppState *st);

static gboolean
action_save_as(AppState *st)
{
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Save VoidDoc As",
        GTK_WINDOW(st->window), GTK_FILE_CHOOSER_ACTION_SAVE,
        "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    add_filters(GTK_FILE_CHOOSER(dlg));
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg),
        st->filepath ? display_name(st) : DEFAULT_FILENAME);

    gboolean saved = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gchar *raw = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gchar *path = ensure_vdoc_extension(raw);
        saved = save_to(st, path);
        g_free(path);
        g_free(raw);
    }
    gtk_widget_destroy(dlg);
    if (saved) update_title(st);
    return saved;
}

static gboolean
action_save(AppState *st)
{
    if (!st->filepath) return action_save_as(st);
    return save_to(st, st->filepath);
}

static void
action_open(AppState *st)
{
    if (!confirm_discard(st)) return;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Open VoidDoc",
        GTK_WINDOW(st->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel", GTK_RESPONSE_CANCEL, "Open", GTK_RESPONSE_ACCEPT, NULL);
    add_filters(GTK_FILE_CHOOSER(dlg));

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        load_file(st, path);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static void
action_new(AppState *st)
{
    if (!confirm_discard(st)) return;
    gtk_text_buffer_set_text(st->buf, "", 0);
    g_free(st->filepath);
    st->filepath = NULL;
    set_modified(st, FALSE);
    update_title(st);
    schedule_highlight(st);
}

static void
action_exit(AppState *st)
{
    if (!confirm_discard(st)) return;
    gtk_widget_destroy(st->window);
}

/* Show/Hide Markup: this is what makes the single page double as
 * both the editor and the preview -- it flips the "invisible"
 * property on the shared "markup" tag, hiding every bit of Rich
 * Void Format punctuation while leaving the rendered formatting
 * (bold/italic/headers/links/...) in place. Both the Home-tab and
 * View-tab buttons drive this and stay in sync. */
static void
action_toggle_markup(AppState *st)
{
    st->show_markup = !st->show_markup;
    if (st->markup_tag)
        g_object_set(st->markup_tag, "invisible", !st->show_markup, NULL);

    const char *label = st->show_markup ? "Hide Markup" : "Show Markup";
    if (st->markup_toggle_btn) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->markup_toggle_btn), !st->show_markup);
        gtk_button_set_label(GTK_BUTTON(st->markup_toggle_btn), label);
    }
    if (st->markup_toggle_btn_view) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->markup_toggle_btn_view), !st->show_markup);
        gtk_button_set_label(GTK_BUTTON(st->markup_toggle_btn_view), label);
    }
}

/* ================================================================
 * Rich Void Text editing commands (Home + Insert tabs)
 * ================================================================ */

static void
wrap_selection(AppState *st, const char *open, const char *close)
{
    GtkTextIter start, end;
    gboolean has_sel = gtk_text_buffer_get_selection_bounds(st->buf, &start, &end);

    if (!has_sel) {
        GtkTextMark *insert = gtk_text_buffer_get_insert(st->buf);
        gtk_text_buffer_get_iter_at_mark(st->buf, &start, insert);
        end = start;
    }

    gchar *sel = gtk_text_buffer_get_text(st->buf, &start, &end, FALSE);
    gchar *repl = g_strdup_printf("%s%s%s", open, sel, close);
    gtk_text_buffer_delete(st->buf, &start, &end);
    gtk_text_buffer_insert(st->buf, &start, repl, -1);
    g_free(repl);
    g_free(sel);
    schedule_highlight(st);
}

static void action_bold(AppState *st)         { wrap_selection(st, "**", "**"); }
static void action_italic(AppState *st)       { wrap_selection(st, "*", "*"); }
static void action_underline(AppState *st)    { wrap_selection(st, "__", "__"); }
static void action_strike(AppState *st)       { wrap_selection(st, "~~", "~~"); }
static void action_highlight(AppState *st)    { wrap_selection(st, "==", "=="); }
static void action_insert_code(AppState *st)  { wrap_selection(st, "`", "`"); }
static void action_superscript(AppState *st)  { wrap_selection(st, "^", "^"); }
static void action_subscript(AppState *st)    { wrap_selection(st, "~", "~"); }

static void
action_insert_link(AppState *st)
{
    GtkTextIter start, end;
    gboolean has_sel = gtk_text_buffer_get_selection_bounds(st->buf, &start, &end);
    gchar *label = has_sel ? gtk_text_buffer_get_text(st->buf, &start, &end, FALSE)
                            : g_strdup("link text");
    gchar *repl = g_strdup_printf("[%s](https://)", label);

    if (has_sel) {
        gtk_text_buffer_delete(st->buf, &start, &end);
    } else {
        GtkTextMark *insert = gtk_text_buffer_get_insert(st->buf);
        gtk_text_buffer_get_iter_at_mark(st->buf, &start, insert);
    }
    gtk_text_buffer_insert(st->buf, &start, repl, -1);
    g_free(repl);
    g_free(label);
    schedule_highlight(st);
}

static void
action_insert_hr(AppState *st)
{
    GtkTextIter iter;
    GtkTextMark *insert = gtk_text_buffer_get_insert(st->buf);
    gtk_text_buffer_get_iter_at_mark(st->buf, &iter, insert);
    gtk_text_buffer_insert(st->buf, &iter, "\n---\n", -1);
    schedule_highlight(st);
}

/* ================================================================
 * Layout / View tab: zoom
 * ================================================================ */

static void
apply_zoom(AppState *st)
{
    PangoFontDescription *desc = pango_font_description_from_string(
        "Consolas, Cascadia Code, monospace");
    pango_font_description_set_size(desc, (gint)(11 * PANGO_SCALE * st->zoom_level));
    gtk_widget_override_font(st->editor_view, desc);
    pango_font_description_free(desc);

    if (st->zoom_label) {
        gchar *pct = g_strdup_printf("%d%%", (int)(st->zoom_level * 100 + 0.5));
        gtk_label_set_text(GTK_LABEL(st->zoom_label), pct);
        g_free(pct);
    }
}

static void action_zoom_in(AppState *st)    { st->zoom_level = MIN(st->zoom_level + 0.1, 3.0); apply_zoom(st); }
static void action_zoom_out(AppState *st)   { st->zoom_level = MAX(st->zoom_level - 0.1, 0.5); apply_zoom(st); }
static void action_zoom_reset(AppState *st) { st->zoom_level = 1.0; apply_zoom(st); }

/* ================================================================
 * Review tab: word count
 * ================================================================ */

static void
action_word_count(AppState *st)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(st->buf, &s, &e);
    gchar *text = gtk_text_buffer_get_text(st->buf, &s, &e, FALSE);

    glong chars = g_utf8_strlen(text, -1);
    int words = 0;
    gboolean in_word = FALSE;
    for (const char *p = text; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (g_unichar_isspace(c)) {
            in_word = FALSE;
        } else if (!in_word) {
            in_word = TRUE;
            words++;
        }
    }
    int lines = gtk_text_buffer_get_line_count(st->buf);
    g_free(text);

    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(st->window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "%d words, %ld characters, %d lines.", words, chars, lines);
    gtk_window_set_title(GTK_WINDOW(dlg), "Word Count");
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* ================================================================
 * UI construction
 * ================================================================ */

static gboolean
on_delete_event(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)w; (void)ev;
    AppState *st = data;
    if (!confirm_discard(st)) return TRUE; /* block close */
    return FALSE;
}

static gboolean
on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    AppState *st = data;
    gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (ev->state & GDK_SHIFT_MASK) != 0;
    if (!ctrl) return FALSE;

    switch (ev->keyval) {
        case GDK_KEY_n: case GDK_KEY_N: action_new(st); return TRUE;
        case GDK_KEY_o: case GDK_KEY_O: action_open(st); return TRUE;
        case GDK_KEY_s: case GDK_KEY_S:
            if (shift) action_save_as(st); else action_save(st);
            return TRUE;
        case GDK_KEY_e: case GDK_KEY_E:
        case GDK_KEY_p: case GDK_KEY_P:
            action_toggle_markup(st); return TRUE;
        case GDK_KEY_k: case GDK_KEY_K:
            action_insert_link(st); return TRUE;
        case GDK_KEY_equal: case GDK_KEY_plus:
            action_zoom_in(st); return TRUE;
        case GDK_KEY_minus:
            action_zoom_out(st); return TRUE;
        default: return FALSE;
    }
}

static GtkWidget *
make_toolbar_button(const char *label, GCallback cb, AppState *st)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_set_name(btn, "vd-toolbtn");
    g_signal_connect_swapped(btn, "clicked", cb, st);
    return btn;
}

/* Small square button for the quick-access strip (New/Open/Save...) */
static GtkWidget *
make_qat_button(const char *label, const char *tooltip, GCallback cb, AppState *st)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_set_name(btn, "vd-qatbtn");
    gtk_widget_set_tooltip_text(btn, tooltip);
    g_signal_connect_swapped(btn, "clicked", cb, st);
    return btn;
}

/* Wraps a row of controls into a captioned ribbon group: the controls
 * on top, a small centered caption ("Font", "Views", ...) below --
 * exactly how Word groups its ribbon commands. */
static GtkWidget *
make_ribbon_group(const char *caption, GtkWidget *body)
{
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_name(group, "vd-group");

    gtk_widget_set_name(body, "vd-group-body");
    gtk_widget_set_valign(body, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(group), body, TRUE, TRUE, 0);

    GtkWidget *cap = gtk_label_new(caption);
    gtk_widget_set_name(cap, "vd-group-caption");
    gtk_box_pack_start(GTK_BOX(group), cap, FALSE, FALSE, 0);

    return group;
}

static GtkWidget *
make_ribbon_separator(void)
{
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_name(sep, "vd-ribbon-sep");
    return sep;
}

/* ---- every ribbon tab's actual command row ---- */

static GtkWidget *
build_file_ribbon(AppState *st)
{
    GtkWidget *ribbon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(ribbon, "vd-ribbon-row");

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_pack_start(GTK_BOX(body), make_toolbar_button("New",     G_CALLBACK(action_new),     st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), make_toolbar_button("Open",    G_CALLBACK(action_open),    st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), make_toolbar_button("Save",    G_CALLBACK(action_save),    st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), make_toolbar_button("Save As", G_CALLBACK(action_save_as), st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_group("File", body), FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_separator(), FALSE, FALSE, 0);

    GtkWidget *close_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_pack_start(GTK_BOX(close_body), make_toolbar_button("Exit", G_CALLBACK(action_exit), st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_group("Close", close_body), FALSE, FALSE, 6);

    return ribbon;
}

static GtkWidget *
build_home_ribbon(AppState *st)
{
    GtkWidget *ribbon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(ribbon, "vd-ribbon-row");

    GtkWidget *doc_group_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_pack_start(GTK_BOX(doc_group_body), make_toolbar_button("New",  G_CALLBACK(action_new),     st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(doc_group_body), make_toolbar_button("Open", G_CALLBACK(action_open),    st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(doc_group_body), make_toolbar_button("Save", G_CALLBACK(action_save),    st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_group("Document", doc_group_body), FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_separator(), FALSE, FALSE, 0);

    GtkWidget *font_group_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GtkWidget *btn_bold = make_toolbar_button("B",  G_CALLBACK(action_bold),      st);
    GtkWidget *btn_ital = make_toolbar_button("I",  G_CALLBACK(action_italic),    st);
    GtkWidget *btn_undl = make_toolbar_button("U",  G_CALLBACK(action_underline), st);
    GtkWidget *btn_strk = make_toolbar_button("S",  G_CALLBACK(action_strike),    st);
    GtkWidget *btn_high = make_toolbar_button("Hl", G_CALLBACK(action_highlight), st);
    gtk_widget_set_name(gtk_bin_get_child(GTK_BIN(btn_bold)), "vd-glyph-bold");
    gtk_widget_set_name(gtk_bin_get_child(GTK_BIN(btn_ital)), "vd-glyph-italic");
    gtk_widget_set_name(gtk_bin_get_child(GTK_BIN(btn_undl)), "vd-glyph-underline");
    gtk_box_pack_start(GTK_BOX(font_group_body), btn_bold, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(font_group_body), btn_ital, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(font_group_body), btn_undl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(font_group_body), btn_strk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(font_group_body), btn_high, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_group("Font", font_group_body), FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_separator(), FALSE, FALSE, 0);

    GtkWidget *view_group_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    st->markup_toggle_btn = gtk_toggle_button_new_with_label("Hide Markup");
    gtk_widget_set_name(st->markup_toggle_btn, "vd-toolbtn");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->markup_toggle_btn), FALSE);
    g_signal_connect_swapped(st->markup_toggle_btn, "clicked", G_CALLBACK(action_toggle_markup), st);
    gtk_box_pack_start(GTK_BOX(view_group_body), st->markup_toggle_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_group("Views", view_group_body), FALSE, FALSE, 6);

    return ribbon;
}

static GtkWidget *
build_insert_ribbon(AppState *st)
{
    GtkWidget *ribbon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(ribbon, "vd-ribbon-row");

    GtkWidget *insert_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_pack_start(GTK_BOX(insert_body), make_toolbar_button("Link", G_CALLBACK(action_insert_link), st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(insert_body), make_toolbar_button("Code", G_CALLBACK(action_insert_code), st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(insert_body), make_toolbar_button("Rule", G_CALLBACK(action_insert_hr),   st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_group("Insert", insert_body), FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_separator(), FALSE, FALSE, 0);

    GtkWidget *script_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_pack_start(GTK_BOX(script_body), make_toolbar_button("x\xc2\xb2",     G_CALLBACK(action_superscript), st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(script_body), make_toolbar_button("x\xe2\x82\x82", G_CALLBACK(action_subscript),   st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_group("Script", script_body), FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_separator(), FALSE, FALSE, 0);

    GtkWidget *markup_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_pack_start(GTK_BOX(markup_body), make_toolbar_button("Highlight", G_CALLBACK(action_highlight), st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_group("Markup", markup_body), FALSE, FALSE, 6);

    return ribbon;
}

static GtkWidget *
build_layout_ribbon(AppState *st)
{
    GtkWidget *ribbon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(ribbon, "vd-ribbon-row");

    GtkWidget *zoom_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_pack_start(GTK_BOX(zoom_body), make_toolbar_button("Zoom \xe2\x88\x92", G_CALLBACK(action_zoom_out),   st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(zoom_body), make_toolbar_button("Zoom +",           G_CALLBACK(action_zoom_in),    st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(zoom_body), make_toolbar_button("Reset",            G_CALLBACK(action_zoom_reset), st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_group("Zoom", zoom_body), FALSE, FALSE, 6);

    return ribbon;
}

static GtkWidget *
build_review_ribbon(AppState *st)
{
    GtkWidget *ribbon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(ribbon, "vd-ribbon-row");

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_pack_start(GTK_BOX(body), make_toolbar_button("Word Count", G_CALLBACK(action_word_count), st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_group("Proofing", body), FALSE, FALSE, 6);

    return ribbon;
}

static GtkWidget *
build_view_ribbon(AppState *st)
{
    GtkWidget *ribbon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(ribbon, "vd-ribbon-row");

    GtkWidget *show_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    st->markup_toggle_btn_view = gtk_toggle_button_new_with_label("Hide Markup");
    gtk_widget_set_name(st->markup_toggle_btn_view, "vd-toolbtn");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->markup_toggle_btn_view), FALSE);
    g_signal_connect_swapped(st->markup_toggle_btn_view, "clicked", G_CALLBACK(action_toggle_markup), st);
    gtk_box_pack_start(GTK_BOX(show_body), st->markup_toggle_btn_view, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_group("Show", show_body), FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_separator(), FALSE, FALSE, 0);

    GtkWidget *zoom_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_pack_start(GTK_BOX(zoom_body), make_toolbar_button("Zoom \xe2\x88\x92", G_CALLBACK(action_zoom_out),   st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(zoom_body), make_toolbar_button("Zoom +",           G_CALLBACK(action_zoom_in),    st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(zoom_body), make_toolbar_button("Reset",            G_CALLBACK(action_zoom_reset), st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ribbon), make_ribbon_group("Zoom", zoom_body), FALSE, FALSE, 6);

    return ribbon;
}

/* Clicking a ribbon tab switches the visible ribbon-stack page and
 * restyles the tab row so exactly one tab looks "active" -- a real,
 * working tab strip instead of decorative labels. */
static void
on_tab_clicked(GtkButton *btn, gpointer data)
{
    AppState *st = data;
    const char *page = g_object_get_data(G_OBJECT(btn), "vd-page");
    if (page) gtk_stack_set_visible_child_name(GTK_STACK(st->ribbon_stack), page);

    for (int i = 0; i < N_TABS; i++) {
        gboolean active = (st->tab_buttons[i] == GTK_WIDGET(btn));
        gtk_widget_set_name(st->tab_buttons[i], active ? "vd-tab-active" : "vd-tab");
    }
}

static GtkWidget *
make_ribbon_tab_button(const char *label, const char *page, gboolean active, AppState *st)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_set_name(btn, active ? "vd-tab-active" : "vd-tab");
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(btn, FALSE);
    g_object_set_data(G_OBJECT(btn), "vd-page", (gpointer)page);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_clicked), st);
    return btn;
}

static void
apply_css(void)
{
    GtkCssProvider *css = gtk_css_provider_new();
    const gchar *style =
        "window { background-color: " CSS_BG "; font-family: 'Segoe UI', 'Aptos', 'Noto Sans', sans-serif; }"
        "label, button { font-family: 'Segoe UI', 'Aptos', 'Noto Sans', sans-serif; }"

        /* -- quick access strip (dark blue, top of window) -- */
        "#vd-titlebar { background-color: " CSS_TITLEBAR "; padding: 5px 10px; }"
        "#vd-qatbtn { background-image: none; background-color: transparent; color: #ffffff;"
        "  border: 1px solid transparent; border-radius: 3px; padding: 3px 9px; font-size: 90%; }"
        "#vd-qatbtn:hover { background-color: " CSS_TITLEBAR_DK "; border-color: rgba(255,255,255,0.35); }"
        "#vd-brand { color: #ffffff; font-weight: 600; font-size: 95%; }"
        "#vd-docname { color: #ffffff; font-size: 90%; opacity: 0.92; }"

        /* -- ribbon tab strip (real, clickable buttons now) -- */
        "#vd-tabstrip { background-color: " CSS_PANEL_BG "; border-bottom: 1px solid " CSS_EDGE "; padding: 0px 6px; }"
        "#vd-tab, #vd-tab-active { border: none; background-image: none; box-shadow: none; outline: none; }"
        "#vd-tab { color: " CSS_TEXT_MUTED "; padding: 6px 12px; font-size: 92%; background-color: transparent; }"
        "#vd-tab:hover { color: " CSS_ACCENT "; }"
        "#vd-tab-active { color: " CSS_ACCENT "; padding: 6px 12px; font-size: 92%; font-weight: 600;"
        "  background-color: " PAGE_BG "; border-top: 2px solid " CSS_ACCENT "; }"

        /* -- ribbon command row -- */
        "#vd-ribbon-stack, #vd-ribbon-row { background-color: " PAGE_BG "; }"
        "#vd-ribbon-row { padding: 6px 10px; border-bottom: 1px solid " CSS_EDGE "; }"
        "#vd-group-caption { color: " CSS_TEXT_MUTED "; font-size: 78%; padding-top: 2px; }"
        "#vd-ribbon-sep { background-color: " CSS_EDGE "; min-width: 1px; margin: 2px 8px; }"

        "#vd-toolbtn { background-image: none; background-color: " PAGE_BG "; color: " CSS_TEXT ";"
        "  border: 1px solid transparent; border-radius: 4px; padding: 5px 11px; min-width: 20px; }"
        "#vd-toolbtn:hover { background-color: " CSS_ACCENT_TINT "; border-color: " CSS_ACCENT_HOV "; }"
        "#vd-toolbtn:checked, #vd-toolbtn:active { background-color: #CFE1F7; border-color: " CSS_ACCENT "; color: " CSS_ACCENT "; }"
        "#vd-glyph-bold { font-weight: 800; font-size: 105%; }"
        "#vd-glyph-italic { font-style: italic; font-size: 105%; }"
        "#vd-glyph-underline { text-decoration-line: underline; font-size: 105%; }"

        /* -- document canvas: gray backdrop behind a white "page" -- */
        "#vd-canvas { background-color: " CSS_BG "; }"
        "#vd-page { background-color: " PAGE_BG "; border: 1px solid " CSS_EDGE ";"
        "  box-shadow: 0 1px 6px rgba(0,0,0,0.22); }"

        "#vd-status { background-color: " CSS_PANEL_BG "; color: " CSS_TEXT_MUTED ";"
        "  padding: 4px 12px; font-size: 88%; border-top: 1px solid " CSS_EDGE "; }"
        "#vd-zoom { color: " CSS_TEXT_MUTED "; padding: 4px 12px; font-size: 88%; }"

        "textview { background-color: " PAGE_BG "; color: " PAGE_TEXT "; caret-color: " CSS_ACCENT "; }"
        "textview text { background-color: " PAGE_BG "; color: " PAGE_TEXT "; }"
        "#vd-editor, #vd-editor text { font-family: 'Aptos', 'Calibri', 'Segoe UI', sans-serif; font-size: 12pt; }";
    gtk_css_provider_load_from_data(css, style, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static AppState *
build_window(GtkApplication *app)
{
    AppState *st = g_new0(AppState, 1);
    st->show_markup = TRUE;
    st->zoom_level = 1.0;

    st->window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(st->window), 860, 620);
    gtk_window_set_icon_name(GTK_WINDOW(st->window), "accessories-text-editor");

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(st->window), root);

    /* ---- quick access strip (dark blue, top of window) ---- */
    GtkWidget *titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(titlebar, "vd-titlebar");
    gtk_box_pack_start(GTK_BOX(root), titlebar, FALSE, FALSE, 0);

    GtkWidget *brand = gtk_label_new("VoidDocs");
    gtk_widget_set_name(brand, "vd-brand");
    gtk_box_pack_start(GTK_BOX(titlebar), brand, FALSE, FALSE, 6);

    GtkWidget *qat_sep = gtk_label_new("\xe2\x94\x82");
    gtk_widget_set_name(qat_sep, "vd-docname");
    gtk_box_pack_start(GTK_BOX(titlebar), qat_sep, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(titlebar), make_qat_button("New",     "New (Ctrl+N)",           G_CALLBACK(action_new),     st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(titlebar), make_qat_button("Open",    "Open (Ctrl+O)",          G_CALLBACK(action_open),    st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(titlebar), make_qat_button("Save",    "Save (Ctrl+S)",          G_CALLBACK(action_save),    st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(titlebar), make_qat_button("Save As", "Save As (Ctrl+Shift+S)", G_CALLBACK(action_save_as), st), FALSE, FALSE, 0);

    GtkWidget *titlebar_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(titlebar), titlebar_spacer, TRUE, TRUE, 0);

    st->titlebar_doc_label = gtk_label_new(DEFAULT_FILENAME);
    gtk_widget_set_name(st->titlebar_doc_label, "vd-docname");
    gtk_box_pack_end(GTK_BOX(titlebar), st->titlebar_doc_label, FALSE, FALSE, 8);

    /* ---- ribbon tab strip: real buttons that switch ribbon pages ---- */
    GtkWidget *tabstrip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_name(tabstrip, "vd-tabstrip");
    gtk_box_pack_start(GTK_BOX(root), tabstrip, FALSE, FALSE, 0);

    /* ---- ribbon: a GtkStack, one real command row per tab ---- */
    st->ribbon_stack = gtk_stack_new();
    gtk_widget_set_name(st->ribbon_stack, "vd-ribbon-stack");
    gtk_stack_set_transition_type(GTK_STACK(st->ribbon_stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_box_pack_start(GTK_BOX(root), st->ribbon_stack, FALSE, FALSE, 0);

    static const char *tab_names[N_TABS]  = { "File", "Home", "Insert", "Layout", "Review", "View" };
    static const char *tab_pages[N_TABS]  = { "file", "home", "insert", "layout", "review", "view" };

    GtkWidget *pages[N_TABS];
    pages[0] = build_file_ribbon(st);
    pages[1] = build_home_ribbon(st);
    pages[2] = build_insert_ribbon(st);
    pages[3] = build_layout_ribbon(st);
    pages[4] = build_review_ribbon(st);
    pages[5] = build_view_ribbon(st);

    for (int i = 0; i < N_TABS; i++) {
        gboolean active = (i == 1); /* Home starts active */
        st->tab_buttons[i] = make_ribbon_tab_button(tab_names[i], tab_pages[i], active, st);
        gtk_box_pack_start(GTK_BOX(tabstrip), st->tab_buttons[i], FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(st->ribbon_stack), pages[i], tab_pages[i]);
    }
    /* NOTE: gtk_stack_set_visible_child_name must be called after the
     * window is shown (see on_activate/on_open) -- calling it here,
     * before the stack's children are realized, is silently ignored
     * by GTK and would leave the first-added tab ("File") visible
     * even though the "Home" tab is styled active. */

    /* ---- document canvas: gray backdrop, single white "page" ----
     * This one page IS both editor and preview: typing shows live
     * formatting, and the Show/Hide Markup toggle (Home or View tab)
     * switches whether the Rich Void Text punctuation is visible. */
    GtkWidget *canvas = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_name(canvas, "vd-canvas");
    gtk_container_set_border_width(GTK_CONTAINER(canvas), 18);
    gtk_box_pack_start(GTK_BOX(root), canvas, TRUE, TRUE, 0);

    GtkWidget *editor_scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(editor_scroller, "vd-page");
    st->editor_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(st->editor_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(st->editor_view), 48);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(st->editor_view), 48);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(st->editor_view), 28);
    gtk_widget_set_name(st->editor_view, "vd-editor");
    gtk_container_add(GTK_CONTAINER(editor_scroller), st->editor_view);
    gtk_box_pack_start(GTK_BOX(canvas), editor_scroller, TRUE, TRUE, 0);

    /* ---- status bar (word-count / zoom, Word style) ---- */
    GtkWidget *statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(root), statusbar, FALSE, FALSE, 0);

    st->status_label = gtk_label_new("");
    gtk_widget_set_name(st->status_label, "vd-status");
    gtk_widget_set_halign(st->status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(statusbar), st->status_label, TRUE, TRUE, 0);

    st->zoom_label = gtk_label_new("100%");
    gtk_widget_set_name(st->zoom_label, "vd-zoom");
    gtk_widget_set_halign(st->zoom_label, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(statusbar), st->zoom_label, FALSE, FALSE, 0);

    st->buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->editor_view));
    install_tags(st->buf, st);

    g_signal_connect(st->buf, "changed", G_CALLBACK(on_buffer_changed), st);
    g_signal_connect(st->window, "delete-event", G_CALLBACK(on_delete_event), st);
    g_signal_connect(st->window, "key-press-event", G_CALLBACK(on_key_press), st);

    update_title(st);
    return st;
}

/* ================================================================
 * GtkApplication plumbing
 * ================================================================ */

static void
on_activate(GtkApplication *app, gpointer data)
{
    (void)data;
    AppState *st = build_window(app);
    gtk_widget_show_all(st->window);
    gtk_stack_set_visible_child_name(GTK_STACK(st->ribbon_stack), "home");
    schedule_highlight(st);
    gtk_widget_grab_focus(st->editor_view);
}

static void
on_open(GtkApplication *app, GFile **files, gint n_files, const gchar *hint, gpointer data)
{
    (void)hint; (void)data;
    AppState *st = build_window(app);
    gtk_widget_show_all(st->window);
    gtk_stack_set_visible_child_name(GTK_STACK(st->ribbon_stack), "home");
    if (n_files > 0) {
        gchar *path = g_file_get_path(files[0]);
        if (path) { load_file(st, path); g_free(path); }
    }
    gtk_widget_grab_focus(st->editor_view);
}

static void
on_startup(GtkApplication *app, gpointer data)
{
    (void)app; (void)data;
    apply_css();
}

int
main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("org.voidos.voiddocs", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}