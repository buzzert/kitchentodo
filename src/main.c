#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <Xm/XmAll.h>

#define MAX_TODOS 128

typedef struct _todo_item_t {
    bool     complete;
    XmString name;
} todo_item_t;

typedef struct _app_state_t {
    Widget      root_widget;
    Widget      list_widget;

    Widget      list_toggle_widgets[MAX_TODOS];
    todo_item_t todo_items[MAX_TODOS];
    unsigned    num_todo_items;
} app_state_t;

static app_state_t g_app_state = { 0 };

void file_menu_callback (Widget w, XtPointer client, XtPointer call);
void add_menu_callback (Widget w, XtPointer client_data, XtPointer call_data);
void add_menu_completion (Widget w, XtPointer client_data, XtPointer call_data);

void reload_data_for_list (Widget list)
{
#if 0
    for (unsigned i = 0; i < 100; i++) {
        Widget check = XmVaCreateToggleButton (list, "Test",
                                               XmCLabelString, XmStringCreateSimple("Test"),
                                               NULL);
        XtManageChild(check);
    }
#endif
}

void add_todo (todo_item_t item)
{
    g_app_state.todo_items[g_app_state.num_todo_items++] = item;

    Widget item_widget = XmVaCreateToggleButton (g_app_state.list_widget, "item",
                                                 XmNlabelString, item.name,
                                                 XmCIndicatorOn, item.complete,
                                                 NULL);
    XtManageChild (item_widget);
}

int main (int argc, char *argv[])
{
    XtAppContext app;

    Widget toplevel = XtVaOpenApplication (&app, "Shopping List", NULL, 0, &argc, argv, NULL,
        sessionShellWidgetClass,
        XmNminWidth, 275,
        XmNminHeight, 325,
        NULL
    );

    Widget root = XtVaCreateManagedWidget ("main_window", xmMainWindowWidgetClass, toplevel, NULL);
    g_app_state.root_widget = root;

    Widget menubar = XmVaCreateSimpleMenuBar (root, "menubar",
        XmVaCASCADEBUTTON, XmStringCreateSimple("File"), 'F',
        NULL
    );

    XmVaCreateSimplePulldownMenu (menubar, "file_menu", 0, file_menu_callback,
                                  XmVaPUSHBUTTON, XmStringCreateSimple ("Add Item..."), 'A', NULL, NULL,
                                  XmVaSEPARATOR,
                                  XmVaPUSHBUTTON, XmStringCreateSimple("Quit"), 'Q', NULL, NULL,
                                  NULL);

    XtManageChild (menubar);

    /* Main Stack */
    Widget main_form = XmVaCreateForm(root, "main_form",
                                      NULL);
    XtManageChild(main_form);


    /* Add Button */
    Widget add_button = XmVaCreatePushButton (main_form, "add_button",
                                              XmNlabelString, XmStringCreateSimple ("+ Add Item"),
                                              XmNleftAttachment, XmATTACH_FORM,
                                              XmNrightAttachment, XmATTACH_FORM,
                                              XmNbottomAttachment, XmATTACH_FORM,
                                              NULL);
    XtAddCallback (add_button, XmNactivateCallback, add_menu_callback, NULL);
    XtManageChild (add_button);

    /* List View */
    Widget list_scroll = XmVaCreateScrolledWindow (main_form, "scroller",
                                                   XmNscrollingPolicy, XmAUTOMATIC,
                                                   XmNleftAttachment, XmATTACH_FORM,
                                                   XmNrightAttachment, XmATTACH_FORM,
                                                   XmNbottomAttachment, XmATTACH_WIDGET,
                                                   XmNtopAttachment, XmATTACH_FORM,
                                                   XmNbottomWidget, add_button,
                                                   NULL);
    XtManageChild (list_scroll);

    Widget list = XmVaCreateRowColumn (list_scroll, "list",
        XmCNumColumns, 1,
        XmCIsHomogeneous, true,
        XmCEntryClass, xmToggleButtonWidgetClass,

        NULL
    );
    XtManageChild (list);
    g_app_state.list_widget = list;
    reload_data_for_list (list);


    XtRealizeWidget (toplevel);
    XtAppMainLoop (app);

    return 0;
}

void file_menu_callback(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void) w;
    (void) call_data;

    int selected_item = (int)client_data;
    if (selected_item == 0) {
        add_menu_callback (w, client_data, call_data);
    } else {
        // Quit
        exit (0);
    }
}

void add_menu_callback (Widget w, XtPointer client_data, XtPointer call_data)
{
    Arg args[] = {
        { XmNselectionLabelString, XmStringCreateSimple ("Item Name:") }
    };
    Widget dialog = XmCreatePromptDialog (g_app_state.root_widget, "Add Item", args, 1);

    // Done callback
    XtAddCallback (dialog, XmNokCallback, add_menu_completion, w);

    // Delete "Help" button
    XtUnmanageChild (XmSelectionBoxGetChild (dialog, XmDIALOG_HELP_BUTTON));

    XtManageChild (dialog);
    XtPopup (XtParent (dialog), XtGrabNone);
}

void add_menu_completion (Widget w, XtPointer client_data, XtPointer call_data)
{
    XmSelectionBoxCallbackStruct *cbs = (XmSelectionBoxCallbackStruct *) call_data;

    XmString item_string = cbs->value;
    todo_item_t item = {
        .complete = false,
        .name     = XmStringCopy (item_string)
    };
    add_todo (item);
}
