// sits in the channel and writes a line to pm-receipts.txt the moment a private message arrives -
// fs.appendFileSync, so nothing hides in a stdout buffer. recipient-side proof for pm testing.
let fs = require("fs");
let path = require("path");
let t = require("./test-helpers.js");

let receipt_file = path.join(__dirname, "pm-receipts.txt");

function receipt(line)
{
    fs.appendFileSync(receipt_file, new Date().toISOString() + " " + line + "\n");
}

t.start_connected_bundle(function(bundle)
{
    receipt("probe connected as " + bundle.read_state().g_local_username);

    let known_contexts = {};

    setInterval(function()
    {
        let state = bundle.read_state();

        state.g_chat_context_array.forEach(function(context)
        {
            if (known_contexts[context.chat_context_id] == null)
            {
                known_contexts[context.chat_context_id] = true;

                if (context.chat_context_id.indexOf("pm") !== -1 || context.chat_context_id.indexOf("direct") !== -1)
                {
                    receipt("PM RECEIVED - context " + context.chat_context_id
                        + " last sender: " + context.last_known_message_sender_username);
                }
            }
            else if (context.last_known_message_sender_username
                && context.last_known_message_sender_username !== known_contexts[context.chat_context_id]
                && context.chat_context_id.indexOf("channel") === -1)
            {
                receipt("PM ACTIVITY - " + context.chat_context_id + " from " + context.last_known_message_sender_username);
            }

            known_contexts[context.chat_context_id] = context.last_known_message_sender_username || true;
        });
    }, 500);
});

setInterval(function() {}, 60000);
