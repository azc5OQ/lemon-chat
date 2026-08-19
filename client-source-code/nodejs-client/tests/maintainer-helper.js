// a second client, run as its own process so it gets its own identity and its own global.postMessage.
// it joins the root channel first and so becomes the channel keys maintainer for the test that
// spawns it. prints MAINTAINER_READY on stdout once it is logged in, then just stays connected.
let t = require("./test-helpers.js");

t.start_connected_bundle(function(bundle)
{
    // stand in as a client somebody is actually looking at. headless node goes idle, and an idle
    // client leaves the channel - so without this the helper cannot hold the maintainer role
    if (typeof bundle.node_set_ui_attached === "function")
    {
        bundle.node_set_ui_attached(true);
    }

    console.log("MAINTAINER_READY " + bundle.read_state().g_local_username);
});

// nothing else to do - the interval only keeps the process alive
setInterval(function() {}, 60000);
