/* VoidDocs -- the VoidOS markdown editor
 *
 * A small GTK3 app for VoidOS's native document format, `.vdoc`
 * (plain markdown text under the hood -- the extension just tells
 * VoidDocs and the desktop's file associations "open this here").
 *
 * Left pane: a plain-text editor with live markdown syntax
 * highlighting (headers, **bold**, *italic*, `code`, lists,
 * blockquotes). Right pane: a live rendered preview, toggleable.
 * No HTML engine involved -- both panes are GtkTextViews, styled
 * with text tags, which keeps this dependency-light and fast.
 *
 * Runs fine standalone under any GTK-capable WM; under voidwm it
 * gets a titlebar + rounded frame for free, and this window's title
 * (kept in sync with the open file) is what voidwm's titlebar shows.
 */
#include <gtk/gtk.h>
#include <string.h>

#define APP_TITLE        "VoidDocs"
#define VDOC_EXTENSION   ".vdoc"
#define DEFAULT_FILENAME "untitled.vdoc"

/* ---- VoidOS "ember" palette, matching src/config.h ---- */
#define CSS_BG          "#150707"
#define CSS_PANEL_BG    "#1c0a09"
#define CSS_EDGE        "#2a0d0c"
#define CSS_TEXT        "#fff3ee"
#define CSS_TEXT_MUTED  "#d8b8ab"
#define CSS_ACCENT      "#ff5a3c"
#define CSS_ACCENT_HOV  "#ff7a5c"
#define CSS_CODE_BG     "#2a1210"

typedef struct {
    GtkWidget     *window;
    GtkWidget     *paned;
    GtkWidget     *editor_view;
    GtkWidget     *preview_view;
    GtkWidget     *preview_scroller;
    GtkWidget     *toggle_preview_btn;
    GtkWidget     *status_label;
    GtkTextBuffer *buf;
    GtkTextBuffer *preview_buf;
    gchar         *filepath;   /* NULL if never saved */
    gboolean       modified;
    gboolean       preview_on;
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
}

static void
set_modified(AppState *st, gboolean m)
{
    if (st->modified == m) return;
    st->modified = m;
    update_title(st);
}

/* ================================================================
 * Tag setup, shared shape/colors for editor + preview
 * ================================================================ */

static void
install_tags(GtkTextBuffer *buf, gboolean is_preview)
{
    gtk_text_buffer_create_tag(buf, "bold",   "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(buf, "italic", "style",  PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(buf, "code",
                                "family", "monospace",
                                "foreground", "#ffcbb8",
                                "background", CSS_CODE_BG, NULL);
    gtk_text_buffer_create_tag(buf, "link",
                                "foreground", CSS_ACCENT_HOV,
                                "underline", PANGO_UNDERLINE_SINGLE, NULL);
    gtk_text_buffer_create_tag(buf, "list-marker", "foreground", CSS_ACCENT, "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(buf, "blockquote-marker", "foreground", CSS_ACCENT, NULL);
    gtk_text_buffer_create_tag(buf, "blockquote",
                                "style", PANGO_STYLE_ITALIC,
                                "foreground", CSS_TEXT_MUTED,
                                "left-margin", is_preview ? 16 : 0, NULL);
    gtk_text_buffer_create_tag(buf, "hr", "foreground", CSS_TEXT_MUTED, NULL);

    static const double scales[7] = { 0, 1.9, 1.6, 1.35, 1.18, 1.05, 1.0 };
    for (int lvl = 1; lvl <= 6; lvl++) {
        char name[16];
        g_snprintf(name, sizeof name, "header%d", lvl);
        gtk_text_buffer_create_tag(buf, name,
                                    "weight", PANGO_WEIGHT_BOLD,
                                    "scale", scales[lvl],
                                    "foreground", lvl <= 2 ? CSS_ACCENT : CSS_TEXT,
                                    "pixels-above-lines", is_preview ? 6 : 0,
                                    "pixels-below-lines", is_preview ? 2 : 0,
                                    NULL);
    }
}

/* insert `len` bytes of `text` at *iter, tagged with up to two named
 * tags (either may be NULL). Advances *iter past the inserted text. */
static void
insert_tagged(GtkTextBuffer *buf, GtkTextIter *iter, const char *text, gssize len,
              const char *tag1, const char *tag2)
{
    if (len == 0) return;
    GtkTextMark *mark = gtk_text_buffer_create_mark(buf, NULL, iter, TRUE);
    gtk_text_buffer_insert(buf, iter, text, len);
    if (tag1 || tag2) {
        GtkTextIter start;
        gtk_text_buffer_get_iter_at_mark(buf, &start, mark);
        if (tag1) gtk_text_buffer_apply_tag_by_name(buf, tag1, &start, iter);
        if (tag2) gtk_text_buffer_apply_tag_by_name(buf, tag2, &start, iter);
    }
    gtk_text_buffer_delete_mark(buf, mark);
}

/* Scan `text` for **bold**, *italic*, `code`, [link](url) spans and
 * insert into `buf` at *iter, applying `extra_tag` (may be NULL,
 * e.g. a header-level or blockquote tag) to every run alongside. */
static void
insert_inline(GtkTextBuffer *buf, GtkTextIter *iter, const char *text, const char *extra_tag)
{
    const char *p = text;
    while (*p) {
        if (p[0] == '*' && p[1] == '*') {
            const char *end = strstr(p + 2, "**");
            if (end && end > p + 2) {
                insert_tagged(buf, iter, p + 2, end - (p + 2), "bold", extra_tag);
                p = end + 2; continue;
            }
        }
        if (p[0] == '`') {
            const char *end = strchr(p + 1, '`');
            if (end && end > p + 1) {
                insert_tagged(buf, iter, p + 1, end - (p + 1), "code", extra_tag);
                p = end + 1; continue;
            }
        }
        if (p[0] == '*') {
            const char *end = strchr(p + 1, '*');
            if (end && end > p + 1) {
                insert_tagged(buf, iter, p + 1, end - (p + 1), "italic", extra_tag);
                p = end + 1; continue;
            }
        }
        if (p[0] == '[') {
            const char *close = strchr(p + 1, ']');
            if (close && close[1] == '(') {
                const char *urlend = strchr(close + 2, ')');
                if (urlend) {
                    insert_tagged(buf, iter, p + 1, close - (p + 1), "link", extra_tag);
                    p = urlend + 1; continue;
                }
            }
        }
        const char *ns = p + 1;
        while (*ns && *ns != '*' && *ns != '`' && *ns != '[') ns++;
        insert_tagged(buf, iter, p, ns - p, extra_tag, NULL);
        p = ns;
    }
}

/* ================================================================
 * Live preview (right pane): reparse the whole document into
 * preview_buf, replacing markdown punctuation with real styling.
 * ================================================================ */

static void
render_preview(AppState *st)
{
    gtk_text_buffer_set_text(st->preview_buf, "", 0);
    GtkTextIter iter;
    gtk_text_buffer_get_start_iter(st->preview_buf, &iter);

    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(st->buf, &s, &e);
    gchar *whole = gtk_text_buffer_get_text(st->buf, &s, &e, FALSE);

    gchar **lines = g_strsplit(whole, "\n", -1);
    gboolean in_code = FALSE;

    for (int i = 0; lines[i]; i++) {
        if (i > 0) insert_tagged(st->preview_buf, &iter, "\n", 1, NULL, NULL);

        const char *line = lines[i];
        const char *p = line;
        while (*p == ' ') p++;

        if (strncmp(p, "```", 3) == 0) {
            in_code = !in_code;
            continue;
        }
        if (in_code) {
            insert_tagged(st->preview_buf, &iter, line, -1, "code", NULL);
            continue;
        }
        if (*p == '#') {
            int level = 0;
            const char *q = p;
            while (*q == '#' && level < 6) { level++; q++; }
            if (*q == ' ') q++;
            char tagname[16];
            g_snprintf(tagname, sizeof tagname, "header%d", level);
            insert_inline(st->preview_buf, &iter, q, tagname);
            continue;
        }
        if (*p == '>') {
            const char *q = p + 1;
            if (*q == ' ') q++;
            insert_tagged(st->preview_buf, &iter, "\xe2\x96\x8e ", -1, "blockquote-marker", NULL);
            insert_inline(st->preview_buf, &iter, q, "blockquote");
            continue;
        }
        if ((p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ') {
            insert_tagged(st->preview_buf, &iter, "\xe2\x80\xa2  ", -1, "list-marker", NULL);
            insert_inline(st->preview_buf, &iter, p + 2, NULL);
            continue;
        }
        {
            const char *q = p;
            while (g_ascii_isdigit(*q)) q++;
            if (q != p && q[0] == '.' && q[1] == ' ') {
                insert_tagged(st->preview_buf, &iter, p, q - p + 1, "list-marker", NULL);
                insert_tagged(st->preview_buf, &iter, " ", 1, NULL, NULL);
                insert_inline(st->preview_buf, &iter, q + 2, NULL);
                continue;
            }
        }
        if (strcmp(p, "---") == 0 || strcmp(p, "***") == 0) {
            insert_tagged(st->preview_buf, &iter,
                           "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                           "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80",
                           -1, "hr", NULL);
            continue;
        }
        insert_inline(st->preview_buf, &iter, line, NULL);
    }

    g_strfreev(lines);
    g_free(whole);
}

/* ================================================================
 * Editor syntax highlighting (applied in place, over st->buf)
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
        char tagname[16];
        g_snprintf(tagname, sizeof tagname, "header%d", level);
        apply_range(buf, line, lead, len, tagname);
        return;
    }
    if (p[0] == '>') {
        apply_range(buf, line, lead, len, "blockquote");
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
            if (j + 1 < len) { apply_range(buf, line, i, j + 2, "bold"); i = j + 2; continue; }
        }
        if (text[i] == '`') {
            int j = i + 1;
            while (j < len && text[j] != '`') j++;
            if (j < len) { apply_range(buf, line, i, j + 1, "code"); i = j + 1; continue; }
        }
        if (text[i] == '*') {
            int j = i + 1;
            while (j < len && text[j] != '*') j++;
            if (j < len && j > i + 1) { apply_range(buf, line, i, j + 1, "italic"); i = j + 1; continue; }
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
        "bold", "italic", "code", "link", "list-marker",
        "blockquote-marker", "blockquote", "hr",
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

    if (st->preview_on) render_preview(st);
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
    GtkFileFilter *vdoc = gtk_file_filter_new();
    gtk_file_filter_set_name(vdoc, "VoidDocs (*.vdoc)");
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

    gtk_file_chooser_set_filter(chooser, vdoc);
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
action_toggle_preview(AppState *st)
{
    st->preview_on = !st->preview_on;
    gtk_widget_set_visible(st->preview_scroller, st->preview_on);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->toggle_preview_btn), st->preview_on);
    if (st->preview_on) render_preview(st);
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
            action_toggle_preview(st); return TRUE;
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

static void
apply_css(void)
{
    GtkCssProvider *css = gtk_css_provider_new();
    const gchar *style =
        "window { background-color: " CSS_BG "; }"
        "#vd-toolbar { background-color: " CSS_PANEL_BG "; padding: 6px; border-bottom: 1px solid " CSS_EDGE "; }"
        "#vd-toolbtn { background-image: none; background-color: " CSS_EDGE "; color: " CSS_TEXT ";"
        "  border: 1px solid " CSS_EDGE "; border-radius: 6px; padding: 4px 10px; }"
        "#vd-toolbtn:hover { background-color: " CSS_ACCENT "; border-color: " CSS_ACCENT "; }"
        "#vd-toolbtn:checked, #vd-toolbtn:active { background-color: " CSS_ACCENT "; color: #150707; }"
        "#vd-status { color: " CSS_TEXT_MUTED "; padding: 3px 10px; font-size: 90%; }"
        "textview { background-color: " CSS_BG "; color: " CSS_TEXT "; caret-color: " CSS_ACCENT "; }"
        "#vd-editor, #vd-editor text { font-family: monospace; font-size: 11pt; }"
        "textview text { background-color: " CSS_BG "; }"
        "#vd-preview textview, #vd-preview textview text { background-color: " CSS_PANEL_BG "; }"
        "paned > separator { background-color: " CSS_EDGE "; min-width: 2px; }";
    gtk_css_provider_load_from_data(css, style, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static AppState *
build_window(GtkApplication *app)
{
    AppState *st = g_new0(AppState, 1);
    st->preview_on = TRUE;

    st->window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(st->window), 860, 620);
    gtk_window_set_icon_name(GTK_WINDOW(st->window), "accessories-text-editor");

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(st->window), root);

    /* toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_name(toolbar, "vd-toolbar");
    gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(toolbar), make_toolbar_button("New", G_CALLBACK(action_new), st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), make_toolbar_button("Open", G_CALLBACK(action_open), st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), make_toolbar_button("Save", G_CALLBACK(action_save), st), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), make_toolbar_button("Save As", G_CALLBACK(action_save_as), st), FALSE, FALSE, 0);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), spacer, TRUE, TRUE, 0);

    st->toggle_preview_btn = gtk_toggle_button_new_with_label("Preview");
    gtk_widget_set_name(st->toggle_preview_btn, "vd-toolbtn");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->toggle_preview_btn), TRUE);
    g_signal_connect_swapped(st->toggle_preview_btn, "clicked", G_CALLBACK(action_toggle_preview), st);
    gtk_box_pack_start(GTK_BOX(toolbar), st->toggle_preview_btn, FALSE, FALSE, 0);

    /* editor + preview panes */
    st->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(root), st->paned, TRUE, TRUE, 0);

    GtkWidget *editor_scroller = gtk_scrolled_window_new(NULL, NULL);
    st->editor_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(st->editor_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(st->editor_view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(st->editor_view), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(st->editor_view), 10);
    gtk_widget_set_name(st->editor_view, "vd-editor");
    gtk_container_add(GTK_CONTAINER(editor_scroller), st->editor_view);
    gtk_paned_pack1(GTK_PANED(st->paned), editor_scroller, TRUE, FALSE);

    st->preview_scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(st->preview_scroller, "vd-preview");
    st->preview_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(st->preview_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(st->preview_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(st->preview_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(st->preview_view), 16);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(st->preview_view), 16);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(st->preview_view), 10);
    gtk_container_add(GTK_CONTAINER(st->preview_scroller), st->preview_view);
    gtk_paned_pack2(GTK_PANED(st->paned), st->preview_scroller, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(st->paned), 430);

    /* status bar */
    st->status_label = gtk_label_new("");
    gtk_widget_set_name(st->status_label, "vd-status");
    gtk_widget_set_halign(st->status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), st->status_label, FALSE, FALSE, 0);

    st->buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->editor_view));
    st->preview_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->preview_view));
    install_tags(st->buf, FALSE);
    install_tags(st->preview_buf, TRUE);

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
    gtk_widget_set_visible(st->preview_scroller, st->preview_on);
    schedule_highlight(st);
    gtk_widget_grab_focus(st->editor_view);
}

static void
on_open(GtkApplication *app, GFile **files, gint n_files, const gchar *hint, gpointer data)
{
    (void)hint; (void)data;
    AppState *st = build_window(app);
    gtk_widget_show_all(st->window);
    gtk_widget_set_visible(st->preview_scroller, st->preview_on);
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
