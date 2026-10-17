// head-menu.js is embedded in template.html along with the other client files
// it is the menu at the left end of the head menu (the strip at the top of the page that also holds the icon buttons
// at its right end): two buttons, "connections" and "bookmarks", each opening a list below it, like the menus of a
// desktop program. "connections" opens the connect window (the connection form, which used to be a page of its own;
// connection.js owns it) or ends the connection; "bookmarks" lists the saved servers, and a click connects to one.
// the group is hidden on android and in the two member list themes (simpledark, bluebell), which keep the connect
// form as a page of its own; the stylesheets do that hiding
// main.js calls head_menu__init once the page exists; connection.js asks for a repaint of the bookmarks list

/**
 * @brief wires the menu buttons, their lists, the close button of the connect window, and the outside click and the escape key that close an open list
 *
 * @return void
 */
function head_menu__init()
{
    let connections_button = document.getElementById("connections-menu-button");
    let bookmarks_button = document.getElementById("bookmarks-menu-button");
    let connect_item = document.getElementById("connections-menu-connect");
    let disconnect_item = document.getElementById("connections-menu-disconnect");
    let close_button = document.getElementById("connect-window-close-button");

    if (connections_button == null || bookmarks_button == null || connect_item == null || disconnect_item == null)
    {
        return;
    }

    connections_button.onclick = function(event)
    {
        event.stopPropagation();
        head_menu__toggle("connections-menu");
    };

    bookmarks_button.onclick = function(event)
    {
        event.stopPropagation();
        head_menu__toggle("bookmarks-menu");
    };

    connect_item.onclick = function()
    {
        head_menu__close_all();

        // while connected, "connect" is greyed out and does nothing: the window opens only without a connection
        if (connect_item.classList.contains("is-disabled") == false)
        {
            connection__show_connect_window();
        }
    };

    // "disconnect" asks first: the entry turns into a question with "yes" and "no", and only "yes" ends the connection
    disconnect_item.onclick = function(event)
    {
        event.stopPropagation();

        if (disconnect_item.classList.contains("is-disabled") == true)
        {
            head_menu__close_all();
            return;
        }

        if (disconnect_item.classList.contains("is-confirming") == false)
        {
            head_menu__show_disconnect_question();
        }
    };

    if (close_button != null)
    {
        close_button.onclick = connection__hide_connect_window;
    }


    // a click anywhere else closes an open list; the buttons stop their own click from reaching here
    document.addEventListener("click", head_menu__close_all);
    document.addEventListener("keydown", function(event)
    {
        if (event.key === "Escape")
        {
            head_menu__close_all();
        }
    });

    // the "disconnect" entry follows every change of the connection state
    g_connection_status_listeners.push(function() { head_menu__refresh_connections_items(); });
    head_menu__refresh_connections_items();
    head_menu__render_bookmarks();
}

/**
 * @brief opens the list of one menu and closes the other; a click on the button of the open menu closes it
 *
 * @param string menu_id -> "connections-menu" or "bookmarks-menu"
 *
 * @return void
 */
function head_menu__toggle(menu_id)
{
    let menu = document.getElementById(menu_id);

    if (menu == null)
    {
        return;
    }

    let is_open = menu.classList.contains("is-open");

    head_menu__close_all();

    if (is_open == true)
    {
        return;
    }

    if (menu_id === "bookmarks-menu")
    {
        head_menu__render_bookmarks();
    }

    menu.classList.add("is-open");
    menu.querySelector(".head-menu-dropdown-list").style.display = "block";
}

/**
 * @brief turns the "disconnect" entry into the question "disconnect from the server?" with "yes" and "no"
 *        "yes" ends the connection and closes the list; "no", a click elsewhere or the escape key put the entry back
 *
 * @return void
 */
function head_menu__show_disconnect_question()
{
    let disconnect_item = document.getElementById("connections-menu-disconnect");
    let question = document.createElement("span");
    let yes = document.createElement("span");
    let no = document.createElement("span");

    if (disconnect_item == null)
    {
        return;
    }

    disconnect_item.classList.add("is-confirming");
    disconnect_item.innerHTML = "";
    question.textContent = "disconnect from the server?";
    yes.className = "head-menu-answer head-menu-answer-yes";
    yes.textContent = "yes";
    no.className = "head-menu-answer";
    no.textContent = "no";

    yes.onclick = function(event)
    {
        event.stopPropagation();
        head_menu__close_all();
        connection__disconnect_by_user();
    };

    no.onclick = function(event)
    {
        event.stopPropagation();
        head_menu__reset_disconnect_item();
    };

    disconnect_item.appendChild(question);
    disconnect_item.appendChild(yes);
    disconnect_item.appendChild(no);
}

/**
 * @brief puts the "disconnect" entry back to its plain form
 *
 * @return void
 */
function head_menu__reset_disconnect_item()
{
    let disconnect_item = document.getElementById("connections-menu-disconnect");

    if (disconnect_item != null && disconnect_item.classList.contains("is-confirming") == true)
    {
        disconnect_item.classList.remove("is-confirming");
        disconnect_item.textContent = "disconnect";
    }
}

/**
 * @brief closes every open list, and takes back an open disconnect question
 *
 * @return void
 */
function head_menu__close_all()
{
    let menus = document.getElementsByClassName("head-menu-dropdown");

    head_menu__reset_disconnect_item();

    for (let i = 0; i < menus.length; i++)
    {
        let list = menus[i].querySelector(".head-menu-dropdown-list");

        menus[i].classList.remove("is-open");

        if (list != null)
        {
            list.style.display = "none";
        }
    }
}

/**
 * @brief the two entries follow the connection: "connect" is greyed out while a connection exists, "disconnect" is greyed out
 *        unless a connection exists or an attempt runs (in the retry countdown it is greyed out too)
 *
 * @return void
 */
function head_menu__refresh_connections_items()
{
    let connect_item = document.getElementById("connections-menu-connect");
    let disconnect_item = document.getElementById("connections-menu-disconnect");

    if (connect_item == null || disconnect_item == null)
    {
        return;
    }

    let is_connected = (g_is_authenticated == true || g_connection_status.state === "connected");
    let is_disconnect_available = (is_connected == true || g_connection_status.state === "connecting");

    connect_item.classList.toggle("is-disabled", is_connected == true);
    disconnect_item.classList.toggle("is-disabled", is_disconnect_available == false);
}

/**
 * @brief whether a connection exists right now, for the entries that must do nothing then
 *
 * @return boolean
 */
function head_menu__is_connected()
{
    return (g_is_authenticated == true || g_connection_status.state === "connected");
}

/**
 * @brief fills the bookmarks list from the saved servers: one row per server, which connects to it, with a delete mark at its right end,
 *        and below them the row that saves the server of the connect window's fields under a name. an empty list shows a note
 *        above that row. connection.js calls this after every change of the saved servers
 *
 * @return void
 */
function head_menu__render_bookmarks()
{
    let list = document.getElementById("bookmarks-menu-list");

    if (list == null || typeof connection__read_server_bookmarks !== "function")
    {
        return;
    }

    let bookmarks = connection__read_server_bookmarks();

    list.innerHTML = "";

    if (bookmarks.length == 0)
    {
        let note = document.createElement("p");

        note.className = "head-menu-dropdown-note";
        note.textContent = "no saved servers";
        list.appendChild(note);
    }

    for (let i = 0; i < bookmarks.length; i++)
    {
        let row = document.createElement("p");
        let label = document.createElement("span");
        let remove = document.createElement("span");

        row.className = "head-menu-dropdown-item bookmarks-menu-item";
        // a bookmark connects, so like "connect" it is greyed out while a connection exists; its delete mark still works
        row.classList.toggle("is-disabled", head_menu__is_connected());
        row.setAttribute("data-bookmark-index", i);
        row.title = bookmarks[i].host + ":" + bookmarks[i].port;
        label.textContent = bookmarks[i].name;
        remove.className = "bookmarks-menu-item-delete";
        remove.textContent = "✕";
        remove.title = "delete this bookmark";
        row.appendChild(label);
        row.appendChild(remove);

        row.onclick = function(event)
        {
            event.stopPropagation();
            head_menu__close_all();

            if (event.target === remove)
            {
                head_menu__delete_bookmark(i);
                return;
            }

            if (head_menu__is_connected() == true)
            {
                return;
            }

            head_menu__connect_to_bookmark(i);
        };

        list.appendChild(row);
    }

    head_menu__append_bookmark_save_row(list);
}

/**
 * @brief the row at the end of the bookmarks list: a name and a save button, which save the address and port of the connect window's fields
 *        clicks and keys inside the row stay inside it, because a click that reaches the page closes the list
 *
 * @param HTMLElement list -> the list the row is appended to
 *
 * @return void
 */
function head_menu__append_bookmark_save_row(list)
{
    let save_row = document.createElement("div");
    let name_input = document.createElement("input");
    let save_button = document.createElement("input");

    save_row.className = "bookmarks-menu-save-row";
    name_input.type = "text";
    name_input.id = "bookmarks-menu-name";
    name_input.maxLength = 40;
    name_input.placeholder = "name this server";
    save_button.type = "button";
    save_button.id = "bookmarks-menu-save";
    save_button.value = "save";
    save_button.title = "saves the address and port of the connect window under this name";

    save_row.onclick = function(event)
    {
        event.stopPropagation();
    };

    name_input.onkeydown = function(event)
    {
        if (event.key === "Enter")
        {
            head_menu__save_bookmark();
        }
    };

    save_button.onclick = function(event)
    {
        event.stopPropagation();
        head_menu__save_bookmark();
    };

    save_row.appendChild(name_input);
    save_row.appendChild(save_button);
    list.appendChild(save_row);
}

/**
 * @brief the save row of the bookmarks list: the address and port of the connect window are saved under the typed name
 *        a saved bookmark repaints the list (connection.js), which empties the name for the next one
 *
 * @return void
 */
function head_menu__save_bookmark()
{
    let name_input = document.getElementById("bookmarks-menu-name");

    connection__save_server_bookmark((name_input != null) ? name_input.value : "",
        document.getElementById("input-ip-address").value, document.getElementById("input-port-number").value);
}

/**
 * @brief a bookmark was clicked: its address and port go into the connect window, a connection attempt to it starts, and the window opens
 *        only without a connection (the row is greyed out otherwise). on a page with the server written into it the bookmark takes
 *        that server's place (connection.js); the page's keys are still the ones sent, a bookmark stores none
 *
 * @param number index -> the bookmark's index in the stored list
 *
 * @return void
 */
function head_menu__connect_to_bookmark(index)
{
    let bookmark = connection__read_server_bookmarks()[index];
    let select_element = document.getElementById("server-bookmark-select");

    if (bookmark == null)
    {
        return;
    }

    document.getElementById("input-ip-address").value = bookmark.host;
    document.getElementById("input-port-number").value = bookmark.port;
    document.getElementById("server-bookmark-name").value = bookmark.name;

    // the dropdown inside the window follows, as if the pick had been made there
    if (select_element != null)
    {
        select_element.value = String(index);
    }

    connection__request_connect("bookmark", { host: bookmark.host, port: bookmark.port });
    connection__show_connect_window();
}

/**
 * @brief the delete mark of a bookmark row: the server is removed from the saved list, and both lists are repainted
 *
 * @param number index -> the bookmark's index in the stored list
 *
 * @return void
 */
function head_menu__delete_bookmark(index)
{
    let bookmarks = connection__read_server_bookmarks();

    if (index < 0 || index >= bookmarks.length)
    {
        return;
    }

    bookmarks.splice(index, 1);
    connection__write_server_bookmarks(bookmarks);
    connection__render_server_bookmarks();
}
