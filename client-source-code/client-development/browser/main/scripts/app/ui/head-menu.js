// provides the connection and bookmark menus. initialized by main.js

/**
 * @brief binds connection and bookmark menus, outside clicks and Escape
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

        // disable Connect while connected
        if (connect_item.classList.contains("is-disabled") == false)
        {
            connection__show_connect_window();
        }
    };

    // confirm before disconnecting
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


    // close menus on outside clicks
    document.addEventListener("click", head_menu__close_all);
    document.addEventListener("keydown", function(event)
    {
        if (event.key === "Escape")
        {
            head_menu__close_all();
        }
    });

    // the "disconnect" entry follows every change of the connection state
    g_connection_status_listeners.push(function()
    {
        head_menu__refresh_connections_items();
    });
    head_menu__refresh_connections_items();
    head_menu__render_bookmarks();
}

/**
 * @brief toggles one menu and closes the other
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
 * @brief asks for confirmation before disconnecting
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
 * @brief restores the Disconnect menu item
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
 * @brief closes open menus and cancels disconnect confirmation
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
 * @brief updates Connect and Disconnect availability from connection state
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
 * @brief checks whether a connection is active
 *
 * @return boolean true when authenticated or the connection phase is connected
 */
function head_menu__is_connected()
{
    return (g_is_authenticated == true || g_connection_status.state === "connected");
}

/**
 * @brief rebuilds saved server rows and the row for adding a bookmark
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
        // disable bookmark connections while connected. keep deletion available
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
 * @brief adds the bookmark name field and Save button
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
 * @brief saves the form's server address and port under the entered bookmark name
 *
 * @return void
 */
function head_menu__save_bookmark()
{
    let name_input = document.getElementById("bookmarks-menu-name");

    connection__save_server_bookmark((name_input != null) ? name_input.value : "", document.getElementById("input-ip-address").value, document.getElementById("input-port-number").value);
}

/**
 * @brief loads a saved server and starts connecting when no connection exists
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
 * @brief deletes a bookmark and refreshes both bookmark lists
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

/**
 * @brief toggles the identity menu and positions it within the window
 *
 * @param object event -> the click event of the button
 *
 * @return void
 */
function head_menu__identity_button_onclick(event)
{
    let list = document.getElementById("identity-menu-list");
    let rect = event.currentTarget.getBoundingClientRect();
    let left = 0;
    let top = 0;

    // stop the page click
    event.stopPropagation();

    // open the list
    head_menu__toggle("identity-menu");

    // closed by this press
    if (document.getElementById("identity-menu").classList.contains("is-open") == false)
    {
        return;
    }

    // stay inside the window
    left = Math.max(4, Math.min(rect.left, utils__viewport_width() - list.offsetWidth * g_position_scale - 4));
    top = rect.bottom + 2;
    if (top + list.offsetHeight * g_position_scale > utils__viewport_height())
    {
        top = rect.top - list.offsetHeight * g_position_scale - 2;
    }

    // place the list
    list.style.left = utils__position_px(Math.round(left));
    list.style.top = utils__position_px(Math.round(top));
}

/**
 * @brief closes the identity menu and opens the selected import or export dialog
 *
 * @param object event -> the click event of the entry
 *
 * @return void
 */
function head_menu__identity_item_onclick(event)
{
    // close the list
    head_menu__close_all();

    // open the window
    if (event.currentTarget.id === "identity-menu-export")
    {
        UI.show_secret_identity_string_onclick(event);
    }
    else
    {
        UI.import_identity_button_onclick();
    }
}
