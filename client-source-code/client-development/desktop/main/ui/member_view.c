// Builds channel member rows shared by the desktop window and game overlay.
#include "ui/member_view.h"
#include "ui/ui_voice.h"
#include "ui/ui_headers.h"
#include "clib.h"

// declarations
static gui_node_t *_member_view_internal__label(gui_node_t *row, const char *klass, const char *text);

/**
 * @brief creates and appends a bounded text label to a member row.
 *
 * @param gui_node_t* row -> parent member row
 * @param const char* klass -> style classes for the new label
 * @param const char* text -> text buffer
 *
 * @return gui_node_t* -> the new scene-owned label, or NULL on allocation failure
 */
static gui_node_t *_member_view_internal__label(gui_node_t *row, const char *klass, const char *text)
{
    gui_node_t *label = NULL_POINTER;

    // create the label
    label = scene__node_new(GUI_NODE_TYPE__TEXT);
    if (label)
    {
        clib__string_copy(label->klass, sizeof(label->klass), klass);
        scene__set_text(label, text, clib__strlen(text));
        scene__add_child(row, label);
    }
    return label;
}

/**
 * @brief replaces the member rows with a validated channel snapshot.
 *
 * @param gui_node_t* container -> scene node whose member rows are replaced
 * @param const desktop_overlay_snapshot_t* snapshot -> bounded channel display snapshot
 *
 * @return void
 */
void desktop_member_view__update(gui_node_t *container, const desktop_overlay_snapshot_t *snapshot)
{
    gui_node_t *child = NULL_POINTER;
    gui_node_t *next = NULL_POINTER;
    uint index = 0;
    const desktop_overlay_member_t *member = NULL_POINTER;
    gui_node_t *row = NULL_POINTER;
    boole speaking = 0;
    boole local = 0;
    const char *klass = NULL_POINTER;
    int64 audio = 0;

    // check the arguments
    if (!container || !desktop_overlay_data__valid(snapshot))
    {
        return;
    }

    // remove the old rows
    child = container->first_child;
    container->first_child = NULL;
    container->last_child = NULL;
    container->child_count = 0;

    // free the old rows
    while (child)
    {
        next = child->next_sibling;
        child->parent = NULL;
        child->next_sibling = NULL;
        scene__node_free(child);
        child = next;
    }

    // create the member rows
    for (index = 0; index < snapshot->member_count; index++)
    {
        // create the row
        member = &snapshot->members[index];
        row = scene__node_new(GUI_NODE_TYPE__DIV);
        if (!row)
        {
            break;
        }

        // choose the row class
        speaking = (member->flags & DESKTOP_OVERLAY_SPEAKING) != 0;
        local = (member->flags & DESKTOP_OVERLAY_LOCAL) != 0;
        klass = local ? (speaking ? "member local speaking" : "member local") : (speaking ? "member speaking" : "member");
        clib__string_copy(row->klass, sizeof(row->klass), klass);

        // add icon and name
        audio = (member->flags & DESKTOP_OVERLAY_DEAFENED) ? 4 : (member->flags & DESKTOP_OVERLAY_MUTED) ? 3 : speaking ? 1 : 2;
        desktop_ui_voice__append(row, audio, (member->flags & DESKTOP_OVERLAY_LOCAL_MUTE) != 0);
        _member_view_internal__label(row, "member-name", member->name);
        scene__add_child(container, row);
    }

    // report an empty channel
    if (!snapshot->member_count)
    {
        _member_view_internal__label(container, "muted", "No channel members");
    }
}
