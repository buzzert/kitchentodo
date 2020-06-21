#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <Xm/XmAll.h>

#define MAX_TODOS 128
#define MAX_LISTS 16
#define MAX_PATH_LEN 512
#define FS_EVENT_BUFSIZE sizeof (struct inotify_event) + NAME_MAX + 1

#define __unused __attribute__ ((unused))

typedef struct _todo_item_t {
    bool          complete;
    char         *label_string;
    unsigned long id;
} todo_item_t;

typedef struct _todo_list_t {
    XmString      list_name;
    Widget        list_widget;
    Widget        tab_button;

    unsigned long last_item_id;

    unsigned long id;
    Widget        list_toggle_widgets[MAX_TODOS];
    todo_item_t   todo_items[MAX_TODOS];
    unsigned      num_todo_items;

    int           watch_descriptor;
} todo_list_t;

typedef struct _app_state_t {
    XtAppContext  app;
    Widget        root_widget;
    Widget        notebook;

    char          store_path[MAX_PATH_LEN];
    todo_list_t   todo_lists[MAX_LISTS];
    unsigned      num_todo_lists;
    unsigned long last_todo_list_id;

    todo_list_t  *selected_list;

    pthread_t     file_watch_thread;
    int           file_watch_inotify_fd;
} app_state_t;

static app_state_t g_app_state = { 0 };

// File menu
enum {
    FILE_MENU_ADD_ITEM,
    FILE_MENU_CLEAR_COMPLETED,
    FILE_MENU_QUIT,

    FILE_MENU_NUM_ITEMS
};

// Lists menu
enum {
    LISTS_MENU_CREATE_LIST,
    LISTS_MENU_DELETE_LIST,
    LISTS_MENU_RENAME_LIST,

    LISTS_MENU_NUM_ITEMS
};

// Action prototypes
void add_todo (todo_list_t *list, todo_item_t item);
void clear_completed (todo_list_t *list);

void add_todo_list (todo_list_t list);
void reload_todo_lists (void);

// Callbacks
void file_menu_callback (Widget, XtPointer, XtPointer);
void add_menu_callback (Widget, XtPointer, XtPointer);
void add_menu_completion (Widget, XtPointer, XtPointer);
void toggle_item_callback (Widget, XtPointer, XtPointer);
void notebook_page_changed_callback (Widget, XtPointer, XtPointer);

void list_menu_callback (Widget, XtPointer, XtPointer);
void add_list_callback (Widget, XtPointer, XtPointer);
void add_list_completion (Widget, XtPointer, XtPointer);
void delete_list_completion (Widget, XtPointer, XtPointer);
void rename_list_completion (Widget, XtPointer, XtPointer);

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

void todo_list_get_path (todo_list_t *list, char *out_path, size_t out_path_len)
{
    char *list_name_chr = (char *) XmStringUnparse (list->list_name,
                                             NULL,
                                             XmCHARSET_TEXT,
                                             XmCHARSET_TEXT,
                                             NULL, 0, XmOUTPUT_ALL);

    snprintf (out_path, out_path_len, "%s/%lu %s", g_app_state.store_path, list->id, list_name_chr);
}

void todo_item_get_path (todo_list_t *list, todo_item_t item, char *out_path, size_t out_path_len)
{
    char store_path[MAX_PATH_LEN];
    todo_list_get_path (list, store_path, MAX_PATH_LEN);
    snprintf (out_path, out_path_len, "%s/%lu", store_path, item.id);
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
    unsigned id = ++g_app_state.last_todo_list_id;

    todo_list_t list = { 0 };
    list.id = id;
    list.list_name = XmStringCopy (name);

    char store_path[MAX_PATH_LEN];
    todo_list_get_path (&list, store_path, MAX_PATH_LEN);
    mkdir (store_path, S_IRWXU);

    return list;
}

void delete_todo_list (todo_list_t *list)
{
    char filepath[MAX_PATH_LEN];
    todo_list_get_path (list, filepath, MAX_PATH_LEN);

    // Stop watching
    inotify_rm_watch (g_app_state.file_watch_inotify_fd, list->watch_descriptor);

    // Delete all sub items
    DIR *dir = opendir (filepath);
    struct dirent *entry = NULL;
    char subitem_path[MAX_PATH_LEN];
    while ( (entry = readdir (dir)) != NULL ) {
        if (entry->d_name[0] == '.') continue;
        snprintf (subitem_path, MAX_PATH_LEN, "%s/%s", filepath, entry->d_name);
        unlink (subitem_path);
    }

    rmdir (filepath);

    XtUnmanageChild (list->tab_button);
    XtUnmanageChild (list->list_widget);

    for (unsigned int i = 0; i < g_app_state.num_todo_lists; i++) {
        if (g_app_state.todo_lists[i].id == list->id) {
            // Move up
            for (unsigned int j = i; j < g_app_state.num_todo_lists - 1; j++) {
                g_app_state.todo_lists[j] = g_app_state.todo_lists[j + 1];
            }

            break;
        }
    }

    g_app_state.num_todo_lists -= 1;

    // Set the current page to the last page
    unsigned int last_page = 0;
    XtVaGetValues (g_app_state.todo_lists[g_app_state.num_todo_lists - 1].tab_button, XmNpageNumber, &last_page, NULL);
    XtVaSetValues (g_app_state.notebook, XmNcurrentPageNumber, last_page, NULL);
}

void rename_todo_list (todo_list_t *list, XmString new_name)
{
    char from_filepath[MAX_PATH_LEN];
    todo_list_get_path (list, from_filepath, MAX_PATH_LEN);

    XmStringFree (list->list_name);
    list->list_name = XmStringCopy (new_name);
    XtVaSetValues (list->tab_button, XmNlabelString, new_name, NULL);

    char to_filepath[MAX_PATH_LEN];
    todo_list_get_path (list, to_filepath, MAX_PATH_LEN);

    rename (from_filepath, to_filepath);
}

void reload_todos_for_list (todo_list_t *list)
{
    char store_path[MAX_PATH_LEN];
    todo_list_get_path (list, store_path, MAX_PATH_LEN);

    DIR *store = opendir (store_path);
    if (!store) {
        fprintf (stderr, "could not open store path at %s\n", store_path);
        exit (1);
    }

    int result = 0;
    todo_item_t item = { 0 };
    char item_path[MAX_PATH_LEN];
    struct dirent *entry = NULL;
    while ( (entry = readdir (store)) != NULL ) {
        if (entry->d_name[0] == '.') continue;
        snprintf (item_path, MAX_PATH_LEN, "%s/%s", store_path, entry->d_name);

        result = parse_todo_item_at_path (item_path, &item);
        if (result == 0) {
            unsigned long id = strtoul (entry->d_name, NULL, 10);
            if (id > list->last_item_id) {
                list->last_item_id = id;
            }

            item.id = id;

            // Check if todo exists first
            todo_item_t *existing_item = NULL;
            Widget       existing_toggle_widget = NULL;
            for (unsigned i = 0; i < list->num_todo_items; i++) {
                todo_item_t *item = &list->todo_items[i];
                if (item->id == id) {
                    existing_item = item;
                    existing_toggle_widget = list->list_toggle_widgets[i];
                    break;
                }
            }

            if (existing_item != NULL) {
                // Update item checkbox state
                existing_item->complete = item.complete;
                XmToggleButtonSetState (existing_toggle_widget, item.complete, false);
            } else {
                add_todo (list, item);
            }
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

    char filename[MAX_PATH_LEN];
    struct dirent *entry = NULL;
    unsigned int num_todos_to_add = 0;
    todo_list_t sorted_lists[MAX_TODOS] = { { 0 } };
    while ( (entry = readdir (list_store)) != NULL ) {
        if (entry->d_name[0] == '.') continue;

        strncpy (filename, entry->d_name, MAX_PATH_LEN);

        char *num = strtok (entry->d_name, " ");
        unsigned long id = strtoul (num, NULL, 10);

        if (id > g_app_state.last_todo_list_id) {
            g_app_state.last_todo_list_id = id;
        }

        char *name = strtok (NULL, "\0");

        todo_list_t *list = &sorted_lists[id]; // should be guaranteed to be unique
        list->id = id;
        list->list_name = XmStringCreateSimple (name);
        num_todos_to_add++;
    }

    unsigned int i = 0;
    while (num_todos_to_add > 0 && i < MAX_TODOS) {
        if (sorted_lists[i].list_name == NULL) {
            i++; continue;
        }

        add_todo_list (sorted_lists[i]);

        i++; num_todos_to_add--;
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

    // Start watching this directory for fs events
    char list_path[MAX_PATH_LEN];
    todo_list_get_path (&list, list_path, MAX_PATH_LEN);
    int result = inotify_add_watch (g_app_state.file_watch_inotify_fd, list_path, IN_MODIFY);
    if (result == -1) {
        fprintf (stderr, "Error watching list dir: %s\n", strerror (errno));
    } else {
        g_app_state.todo_lists[index].watch_descriptor = result;
    }
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

void* file_watcher_thread_main (__unused void *context)
{
    char buffer[FS_EVENT_BUFSIZE] __attribute__ ((aligned(8))) = { 0 };
    for (;;) {
        ssize_t result = read (g_app_state.file_watch_inotify_fd, buffer, FS_EVENT_BUFSIZE);
        if (result <= 0) {
            fprintf (stderr, "File watcher inotify read error, exiting (%ld)\n", result);
            break;
        }

        struct inotify_event *event = (struct inotify_event *)buffer;
        if (event == NULL) {
            continue;
        }

        // Locate relevant watch descriptor
        for (unsigned int i = 0; i < g_app_state.num_todo_lists; i++) {
            todo_list_t *list = &g_app_state.todo_lists[i];
            if (list->watch_descriptor == event->wd) {
                reload_todos_for_list (list);
                break;
            }
        }
    }

    return NULL;
}

int main (int argc, char *argv[])
{
    initialize_store_if_necessary ();

    /* Initialize Application */
    Widget toplevel = XtVaOpenApplication (&g_app_state.app, "Shopping List", NULL, 0, &argc, argv, NULL,
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
        XmVaCASCADEBUTTON, XmStringCreateSimple ("List"), 'L',
        NULL
    );

    /* File menu */
    XmVaCreateSimplePulldownMenu (menubar, "file_menu", 0, file_menu_callback,
        XmVaPUSHBUTTON, XmStringCreateSimple ("Add Item..."), 'A', "Ctrl<Key>N", XmStringCreateSimple ("Ctrl+N"),
        XmVaPUSHBUTTON, XmStringCreateSimple ("Clear Completed"), 'C', "Ctrl<Key>X", XmStringCreateSimple ("Ctrl+X"),
        XmVaSEPARATOR,
        XmVaPUSHBUTTON, XmStringCreateSimple ("Quit"), 'Q', "Ctrl<Key>Q", XmStringCreateSimple ("Ctrl+Q"),
        NULL);

    XmVaCreateSimplePulldownMenu (menubar, "lists_menu", 1, list_menu_callback,
        XmVaPUSHBUTTON, XmStringCreateSimple ("Create List..."), 'C', NULL, NULL,
        XmVaPUSHBUTTON, XmStringCreateSimple ("Delete List..."), 'D', NULL, NULL,
        XmVaPUSHBUTTON, XmStringCreateSimple ("Rename List..."), 'R', NULL, NULL,
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

    // Set up file watcher
    g_app_state.file_watch_inotify_fd = inotify_init ();
    pthread_create (&g_app_state.file_watch_thread, NULL, file_watcher_thread_main, NULL);

    reload_todo_lists ();

    XtRealizeWidget (toplevel);
    XtAppMainLoop (g_app_state.app);

    // Stop watching file events
    close (g_app_state.file_watch_inotify_fd);
    pthread_join (g_app_state.file_watch_thread, NULL);

    return 0;
}

Widget show_textfield_dialog (XmString title, XmString prompt, XtCallbackProc ok_callback)
{
    Arg args[] = {
        { XmNselectionLabelString, (XtArgVal) prompt },
        { XmNdialogTitle, (XtArgVal) title }
    };

    Widget dialog = XmCreatePromptDialog (g_app_state.root_widget, "dialog", args, 2);

    // Done callback
    XtAddCallback (dialog, XmNokCallback, ok_callback, NULL);

    // Delete "Help" button
    XtUnmanageChild (XmSelectionBoxGetChild (dialog, XmDIALOG_HELP_BUTTON));

    XtManageChild (dialog);
    XtPopup (XtParent (dialog), XtGrabNone);

    return dialog;
}

void show_delete_list_dialog ()
{
    Arg args[] = {
        { XmNdialogTitle, (XtArgVal) XmStringCreateSimple ("Delete List") },
        { XmNmessageString, (XtArgVal) XmStringCreateSimple ("Are you sure?") },
    };

    Widget dialog = XmCreateMessageDialog (g_app_state.root_widget, "dialog", args, 2);

    // Done callback
    XtAddCallback (dialog, XmNokCallback, delete_list_completion, NULL);

    // Remove help button
    XtUnmanageChild (XmMessageBoxGetChild (dialog, XmDIALOG_HELP_BUTTON));

    XtManageChild (dialog);
    XtPopup (XtParent (dialog), XtGrabNone);
}

void show_rename_dialog ()
{
    XmString title = XmStringCreateSimple ("Rename List");
    XmString prompt = XmStringCreateSimple ("List Name: ");
    Widget dialog = show_textfield_dialog (title, prompt, rename_list_completion);
    XtVaSetValues (dialog, XmNtextString, g_app_state.selected_list->list_name, NULL);

    XmStringFree (title);
    XmStringFree (prompt);
}

void file_menu_callback(__unused Widget w,
                        XtPointer client_data,
                        __unused XtPointer call_data)
{
    unsigned long selected_item = (unsigned long) client_data;
    if (selected_item == FILE_MENU_ADD_ITEM) {
        add_menu_callback (w, client_data, call_data);
    } else if (selected_item == FILE_MENU_CLEAR_COMPLETED) {
        clear_completed (g_app_state.selected_list);
    } else {
        // Quit
        exit (0);
    }
}

void add_menu_callback (__unused Widget w,
                        __unused XtPointer client_data,
                        __unused XtPointer call_data)
{
    XmString title = XmStringCreateSimple ("Add Item");
    XmString prompt = XmStringCreateSimple ("Item Name: ");
    show_textfield_dialog (title, prompt, add_menu_completion);

    XmStringFree (title);
    XmStringFree (prompt);
}

void add_menu_completion (__unused Widget w,
                          __unused XtPointer client_data,
                          XtPointer call_data)
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

void toggle_item_callback (Widget w,
                           __unused XtPointer client_data,
                           XtPointer call_data)
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

void notebook_page_changed_callback (__unused Widget w,
                                     __unused XtPointer client_data,
                                     XtPointer call_data)
{
    XmNotebookCallbackStruct *cbs = (XmNotebookCallbackStruct *) call_data;

    // 1 indexed
    unsigned todo_list_idx = 0;
    unsigned selected_list_idx = cbs->page_number;
    for (unsigned int i = 0; i < g_app_state.num_todo_lists; i++) {
        unsigned page_idx = 0;
        XtVaGetValues (g_app_state.todo_lists[i].tab_button, XmNpageNumber, &page_idx, NULL);
        if (page_idx == selected_list_idx) {
            todo_list_idx = i;
            break;
        }
    }

    g_app_state.selected_list = &g_app_state.todo_lists[todo_list_idx];
}

void list_menu_callback (Widget w, XtPointer client_data, XtPointer call_data)
{
    unsigned long selected_item = (unsigned long) client_data;
    if (selected_item == LISTS_MENU_CREATE_LIST) {
        add_list_callback (w, client_data, call_data);
    } else if (selected_item == LISTS_MENU_DELETE_LIST) {
        show_delete_list_dialog ();
    } else if (selected_item == LISTS_MENU_RENAME_LIST) {
        show_rename_dialog ();
    }
}

void add_list_callback (__unused Widget w,
                        __unused XtPointer client_data,
                        __unused XtPointer call_data)
{
    XmString title = XmStringCreateSimple ("Add List");
    XmString prompt = XmStringCreateSimple ("List Name: ");
    show_textfield_dialog (title, prompt, add_list_completion);

    XmStringFree (title);
    XmStringFree (prompt);
}

void add_list_completion (__unused Widget w,
                          __unused XtPointer client_data,
                          XtPointer call_data)
{
    XmSelectionBoxCallbackStruct *cbs = (XmSelectionBoxCallbackStruct *) call_data;

    XmString list_name = cbs->value;
    todo_list_t list = create_todo_list (list_name);
    add_todo_list (list);

    // Select newly created list
    unsigned last_page = 0;
    XtVaGetValues (g_app_state.notebook, XmNlastPageNumber, &last_page, NULL);
    XtVaSetValues (g_app_state.notebook, XmNcurrentPageNumber, last_page, NULL);
}

void delete_list_completion (__unused Widget w,
                             __unused XtPointer client_data,
                             __unused XtPointer call_data)
{
    delete_todo_list (g_app_state.selected_list);
}

void rename_list_completion (__unused Widget w,
                             __unused XtPointer client_data,
                             __unused XtPointer call_data)
{
    XmSelectionBoxCallbackStruct *cbs = (XmSelectionBoxCallbackStruct *) call_data;
    rename_todo_list (g_app_state.selected_list, cbs->value);
}
