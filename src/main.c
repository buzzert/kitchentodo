#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <Xm/XmAll.h>

#define MAX_TODOS 128
#define MAX_LISTS 16
#define MAX_PATH_LEN 512

typedef struct _todo_item_t {
    bool          complete;
    char         *label_string;
    unsigned long id;
} todo_item_t;

typedef struct _todo_list_t {
    XmString      list_name;
    Widget        form_widget;
    Widget        list_widget;
    Widget        tab_button;

    char          store_path[MAX_PATH_LEN];
    unsigned long last_item_id;

    unsigned long id;
    Widget        list_toggle_widgets[MAX_TODOS];
    todo_item_t   todo_items[MAX_TODOS];
    unsigned      num_todo_items;
} todo_list_t;

typedef struct _app_state_t {
    Widget        root_widget;
    Widget        notebook;

    char          store_path[MAX_PATH_LEN];
    todo_list_t   todo_lists[MAX_LISTS];
    unsigned      num_todo_lists;

    todo_list_t  *selected_list;
} app_state_t;

static app_state_t g_app_state = { 0 };

// Action prototypes
void add_todo (todo_list_t *list, todo_item_t item);
void clear_completed (todo_list_t *list);

void add_todo_list (todo_list_t list);
void reload_todo_lists (void);

// Callbacks
void file_menu_callback (Widget w, XtPointer client, XtPointer call);
void add_menu_callback (Widget w, XtPointer client_data, XtPointer call_data);
void add_menu_completion (Widget w, XtPointer client_data, XtPointer call_data);
void toggle_item_callback (Widget w, XtPointer client_data, XtPointer call_data);
void notebook_page_changed_callback (Widget w, XtPointer client_data, XtPointer call_data);

void initialize_store_if_necessary ()
{
    char *home_dir = getenv ("HOME");
    if (!home_dir) {
        fprintf (stderr, "Unable to get $HOME\n");
        exit (1);
    }

    snprintf (g_app_state.store_path, MAX_PATH_LEN, "%s/.local/share/kitchentodo", home_dir);

    struct stat stat_buf;
    if (stat (g_app_state.store_path, &stat_buf) != 0) {
        // Make directory
        int result = mkdir (g_app_state.store_path, S_IRWXU);
        if (result != 0) {
            fprintf (stderr, "Unable to create store path at %s\n", g_app_state.store_path);
            exit (1);
        }
    }
}

int parse_todo_item_at_path (const char *path, todo_item_t *item_out)
{
    struct stat stat_buf;
    if (stat (path, &stat_buf) != 0) {
        return -1;
    }

    enum {
        COMPLETION_STATE,
        TODO_NAME,
        METADATA
    } read_state = COMPLETION_STATE;

    FILE *fp = fopen (path, "r");
    const size_t buf_size = 512;
    char buf[buf_size];
    int read_result = 0;
    while ( (read_result = fread (buf, 1, buf_size, fp)) > 0 ) {
        char *line;
        char *str = buf;
        while ( (line = strtok (str, "\n")) != NULL ) {
            str = NULL; // for each successive call to strtok

            switch (read_state) {
            case COMPLETION_STATE:
                item_out->complete = (line[0] == '1');
                break;
            case TODO_NAME:
                item_out->label_string = malloc (sizeof (char) * strlen (line));
                strcpy (item_out->label_string, line);
                break;
            default:
                break;
            }

            read_state++;
        }
    }

    fclose (fp);
    return 0;
}

void todo_item_get_path (todo_list_t *list, todo_item_t item, char *out_path, size_t out_path_len)
{
    snprintf (out_path, out_path_len, "%s/%lu", list->store_path, item.id);
}

void todo_list_get_path (todo_list_t *list, char *out_path, size_t out_path_len)
{
    char *list_name_chr = (char *) XmStringUnparse (list->list_name,
                                             NULL,
                                             XmCHARSET_TEXT,
                                             XmCHARSET_TEXT,
                                             NULL, 0, XmOUTPUT_ALL);

    snprintf (out_path, out_path_len, "%s/%lu %s", g_app_state.store_path, list->id, list_name_chr);
}

int write_todo_item_to_store (todo_list_t *list, todo_item_t item)
{
    char filename[MAX_PATH_LEN];
    todo_item_get_path (list, item, filename, MAX_PATH_LEN);

    FILE *fp = fopen (filename, "w");
    if (!fp) {
        fprintf (stderr, "Unable to open file for writing: %s\n", filename);
        exit (1);
    }

    fprintf (fp, "%d\n%s\n",
        (item.complete ? 1 : 0),
        item.label_string
    );

    fclose (fp);
    return 0;
}

todo_list_t create_todo_list (XmString name)
{
    unsigned id = g_app_state.num_todo_lists;

    todo_list_t list = { 0 };
    list.id = id;
    list.list_name = name;
    todo_list_get_path (&list, list.store_path, MAX_PATH_LEN);
    mkdir (list.store_path, S_IRWXU);

    return list;
}

void reload_todos_for_list (todo_list_t *list)
{
    DIR *store = opendir (list->store_path);
    if (!store) {
        fprintf (stderr, "could not open store path at %s\n", list->store_path);
        exit (1);
    }

    int result = 0;
    todo_item_t item = { 0 };
    char item_path[MAX_PATH_LEN];
    struct dirent *entry = NULL;
    while ( (entry = readdir (store)) != NULL ) {
        if (entry->d_name[0] == '.') continue;
        snprintf (item_path, MAX_PATH_LEN, "%s/%s", list->store_path, entry->d_name);

        result = parse_todo_item_at_path (item_path, &item);
        if (result == 0) {
            unsigned long id = strtoul (entry->d_name, NULL, 10);
            if (id > list->last_item_id) {
                list->last_item_id = id;
            }

            item.id = id;

            add_todo (list, item);
        }
    }

    closedir (store);
}

void reload_todo_lists ()
{
    DIR *list_store = opendir (g_app_state.store_path);
    if (!list_store) {
        fprintf (stderr, "could not open list store path at %s\n", g_app_state.store_path);
        exit (1);
    }

    todo_list_t list = { 0 };
    struct dirent *entry = NULL;
    while ( (entry = readdir (list_store)) != NULL ) {
        if (entry->d_name[0] == '.') continue;

        snprintf (list.store_path, MAX_PATH_LEN, "%s/%s", g_app_state.store_path, entry->d_name);

        // Parse name
        char *num = strtok (entry->d_name, " ");
        list.id = strtoul (num, NULL, 10);

        char *name = strtok (NULL, " ");
        list.list_name = XmStringCreateSimple (name);

        add_todo_list (list);
    }

    // If there are no todo lists in the store, create the default one
    if (g_app_state.num_todo_lists == 0) {
        todo_list_t default_list = create_todo_list (XmStringCreateSimple ("Todo"));
        add_todo_list (default_list);
    }
}

void add_todo (todo_list_t *list, todo_item_t item)
{
    unsigned int index = list->num_todo_items++;
    list->todo_items[index] = item;

    XmString label_string = XmStringCreateSimple (item.label_string);
    Widget item_widget = XmVaCreateToggleButton (list->list_widget, "item",
                                                 XmNlabelString, label_string,
                                                 XmNset, item.complete,
                                                 XmNuserData, item.id,
                                                 NULL);
    XtAddCallback (item_widget, XmNvalueChangedCallback, toggle_item_callback, NULL);
    XtManageChild (item_widget);
    XmStringFree (label_string);

    list->list_toggle_widgets[index] = item_widget;
}

void add_todo_list (todo_list_t list)
{
    Widget notebook = g_app_state.notebook;

    /* List View */
    Widget list_scroll = XmVaCreateScrolledWindow (notebook, "scroller",
                                                   XmNscrollingPolicy, XmAUTOMATIC,
                                                   NULL);
    XtManageChild (list_scroll);

    Widget list_widget = XmVaCreateRowColumn (list_scroll, "list",
                                              XmCNumColumns, 1,
                                              XmCIsHomogeneous, true,
                                              XmCEntryClass, xmToggleButtonWidgetClass,
                                              NULL);
    XtManageChild (list_widget);

    list.list_widget = list_widget;

    Widget tab = XmVaCreatePushButton (notebook, "tab",
                                       XmNlabelString, list.list_name,
                                       NULL);
    XtManageChild (tab);

    list.tab_button = tab;

    unsigned index = g_app_state.num_todo_lists;
    g_app_state.todo_lists[index] = list;
    g_app_state.selected_list = &g_app_state.todo_lists[index];
    g_app_state.num_todo_lists++;

    reload_todos_for_list (&g_app_state.todo_lists[index]);
}

void clear_completed (todo_list_t *list)
{
    static const unsigned long ID_SENTINEL = ULONG_MAX;

    char filepath[MAX_PATH_LEN];
    unsigned int num_removed = 0;
    for (unsigned int i = 0; i < list->num_todo_items; i++) {
        todo_item_t item = list->todo_items[i];
        if (item.complete) {
            XtUnmanageChild (list->list_toggle_widgets[i]);
            list->list_toggle_widgets[i] = NULL;

            // Delete file in store
            todo_item_get_path (list, item, filepath, MAX_PATH_LEN);
            unlink (filepath);
            
            // Remove item
            num_removed++;
            list->todo_items[i].id = ID_SENTINEL; // queue for deletion below
            free (item.label_string);
        }
    }

    // Close holes
    // Not very efficient... would be better off with a doubly-linked list here. 
    for (unsigned i = 0; i < list->num_todo_items; i++) {
        if (list->todo_items[i].id == ID_SENTINEL) {
            for (unsigned repl = i; repl < list->num_todo_items; repl++) {
                list->todo_items[repl] = list->todo_items[repl + 1];
                list->list_toggle_widgets[repl] = list->list_toggle_widgets[repl + 1];
            }

            list->num_todo_items--;
            i--;
        }
    }
}

int main (int argc, char *argv[])
{
    initialize_store_if_necessary ();

    /* Initialize Application */
    XtAppContext app;
    Widget toplevel = XtVaOpenApplication (&app, "Shopping List", NULL, 0, &argc, argv, NULL,
        sessionShellWidgetClass,
        XmNminWidth, 275,
        XmNminHeight, 325,
        NULL
    );

    /* Create root widget */
    Widget root = XtVaCreateManagedWidget ("main_window", xmMainWindowWidgetClass, toplevel, NULL);
    g_app_state.root_widget = root;

    /* Menu bar */
    Widget menubar = XmVaCreateSimpleMenuBar (root, "menubar",
        XmVaCASCADEBUTTON, XmStringCreateSimple("File"), 'F',
        NULL
    );

    /* File menu */
    XmVaCreateSimplePulldownMenu (menubar, "file_menu", 0, file_menu_callback,
        XmVaPUSHBUTTON, XmStringCreateSimple ("Add Item..."), 'A', "Ctrl<Key>N", XmStringCreateSimple ("Ctrl+N"),
        XmVaPUSHBUTTON, XmStringCreateSimple ("Clear Completed"), 'C', "Ctrl<Key>X", XmStringCreateSimple ("Ctrl+X"),
        XmVaSEPARATOR,
        XmVaPUSHBUTTON, XmStringCreateSimple ("Quit"), 'Q', "Ctrl<Key>Q", XmStringCreateSimple ("Ctrl+Q"),
        NULL);

    XtManageChild (menubar);

    /* Main Stack */
    Widget main_form = XmVaCreateForm(root, "main_form",
                                      NULL);
    XtManageChild(main_form);


    /* Add Button */
    /* Important to make sure this button is managed before the list scroll, so the list scroll can */
    /* reference the add button as its "bottom widget". */
    Widget add_button = XmVaCreatePushButton (main_form, "add_button",
                                              XmNlabelString, XmStringCreateSimple ("+ Add Item"),
                                              XmNleftAttachment, XmATTACH_FORM,
                                              XmNrightAttachment, XmATTACH_FORM,
                                              XmNbottomAttachment, XmATTACH_FORM,
                                              NULL);
    XtAddCallback (add_button, XmNactivateCallback, add_menu_callback, NULL);
    XtManageChild (add_button);

    /* Notebook */
    Widget notebook = XmVaCreateNotebook (main_form, "notebook",
                                          XmNorientation, XmVERTICAL,
                                          XmNbindingType, XmNONE,
                                          XmNbackPagePlacement, XmTOP_RIGHT,
                                          XmNleftAttachment, XmATTACH_FORM,
                                          XmNrightAttachment, XmATTACH_FORM,
                                          XmNbottomAttachment, XmATTACH_WIDGET,
                                          XmNtopAttachment, XmATTACH_FORM,
                                          XmNbottomWidget, add_button,
                                          NULL);
    XtAddCallback (notebook, XmNpageChangedCallback, notebook_page_changed_callback, NULL);
    XtManageChild (notebook);
    g_app_state.notebook = notebook;

    // Remove "page scroller" widget from notebook
    Widget scroller = XtNameToWidget (notebook, "PageScroller");
    XtUnmanageChild (scroller);

    reload_todo_lists ();

    XtRealizeWidget (toplevel);
    XtAppMainLoop (app);

    return 0;
}

void file_menu_callback(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void) w;
    (void) call_data;

    unsigned long selected_item = (unsigned long) client_data;
    if (selected_item == 0) {
        add_menu_callback (w, client_data, call_data);
    } else if (selected_item == 1) {
        clear_completed (g_app_state.selected_list);
    } else {
        // Quit
        exit (0);
    }
}

void add_menu_callback (Widget w, XtPointer client_data, XtPointer call_data)
{
    XmString selection_label_str = XmStringCreateSimple ("Item Name:");
    Arg args[] = {
        { XmNselectionLabelString, (XtArgVal) selection_label_str }
    };
    Widget dialog = XmCreatePromptDialog (g_app_state.root_widget, "Add Item", args, 1);

    // Done callback
    XtAddCallback (dialog, XmNokCallback, add_menu_completion, w);

    // Delete "Help" button
    XtUnmanageChild (XmSelectionBoxGetChild (dialog, XmDIALOG_HELP_BUTTON));

    XtManageChild (dialog);
    XtPopup (XtParent (dialog), XtGrabNone);
    XmStringFree (selection_label_str);
}

void add_menu_completion (Widget w, XtPointer client_data, XtPointer call_data)
{
    XmSelectionBoxCallbackStruct *cbs = (XmSelectionBoxCallbackStruct *) call_data;

    char *item_string = (char *) XmStringUnparse (cbs->value,
                                             XmFONTLIST_DEFAULT_TAG,
                                             XmCHARSET_TEXT,
                                             XmCHARSET_TEXT,
                                             NULL, 0, XmOUTPUT_ALL);

    if (strlen (item_string) > 0) {
        g_app_state.selected_list->last_item_id++;
        todo_item_t item = {
            .complete = false,
            .label_string = item_string,
            .id = g_app_state.selected_list->last_item_id,
        };
        add_todo (g_app_state.selected_list, item);
        write_todo_item_to_store (g_app_state.selected_list, item);
    }
}

void toggle_item_callback (Widget w, XtPointer client_data, XtPointer call_data)
{
    unsigned long item_id;
    XtVaGetValues (w, XmNuserData, &item_id, NULL);

    todo_list_t *list = g_app_state.selected_list;

    todo_item_t *item = NULL;
    for (unsigned int i = 0; i < list->num_todo_items; i++) {
        if (list->todo_items[i].id == item_id) {
            item = &list->todo_items[i];
            break;
        }
    }

    if (item != NULL) {
        XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *) call_data;
        item->complete = cbs->set;
        write_todo_item_to_store (g_app_state.selected_list, *item);
    }
}

void notebook_page_changed_callback (Widget w, XtPointer client_data, XtPointer call_data)
{
    XmNotebookCallbackStruct *cbs = (XmNotebookCallbackStruct *) call_data;

    // 1 indexed
    unsigned selected_list_idx = cbs->page_number - 1;
    g_app_state.selected_list = &g_app_state.todo_lists[selected_list_idx];
}

