mod audio_channel;
mod stun_server;

use chrono;
use base64::DecodeError;
use aes::cipher::{KeyIvInit, StreamCipher};
use sha2::{Digest, Sha256};
use serde_json;
use simple_websockets::{Event, Message, Responder};
use std::collections::HashMap;
use std::sync::{Arc, RwLock, RwLockReadGuard, RwLockWriteGuard};
use std::sync::mpsc::{Sender, Receiver, SyncSender};
use serde_json::{Map, Value};
use std::sync::atomic::{AtomicUsize, Ordering};
use lazy_static::lazy_static;
use std::io::Write;
use rsa::{PublicKey, RsaPublicKey, BigUint, Pkcs1v15Encrypt};
use rand::{distributions::Alphanumeric, Rng};

// 0.8
type AesCtr = ctr::Ctr128BE<aes::Aes256>;


#[derive(Default)]
struct ChatMessageEntry {
    message_id: usize,
    message_type: i8,    //1 channel, 2 private
    receiver_id: u64
}

#[derive(Default)]
struct Tag {
    id: u64,
    icon_id: u64,
    name: String
}

#[derive(Default)]
struct Icon {
    id: u64,
    base64_icon: String
}

//
//if someone wishes to use just chat without tags make it possible to disable it
//

//
//data of clients are linked to public keys..
//

#[derive(Default)]
struct ClientStoredData {
    public_key: String,
    tag_ids: Vec<u64>
}


#[derive(Default)]
struct Client {
    client_id: u64,
    is_authenticated: bool,
    is_admin: bool,
    timestamp_connected: i64,
    //timestamp_last_sent_chat_message: i64,
    timestamp_username_changed: i64,
    timestamp_last_sent_check_connection_message: i64,
    timestamp_last_sent_join_channel_request: i64,
    //timestamp_last_sent_delete_channel_request: i64,
    timestamp_last_channel_created: i64,
    username: String,
    public_key: String,
    is_public_key_challenge_sent: bool,
    public_key_challenge_random_string: String,
    channel_id: u64,
    microphone_state: u64, //1 -> active, 2 -> not active bud enabled, 3 -> disabled audio still active, 4 audio disabled
    is_webrtc_datachannel_connected: bool,
    is_existing: bool,
    message_ids: Vec<ChatMessageEntry>,
    tag_ids: Vec<u64>,
    is_streaming_song: bool,
    song_name: String,
}

#[derive(Default)]
struct Channel
{
    is_channel: bool,
    channel_id: u64,
    parent_channel_id: i64,
    name: String,
    password: String,
    description: String,
    is_using_password: bool,
    maintainer_id: u64,
    is_channel_maintainer_present: bool,
   // num_clients_in_channel: u64
}

//enum MicrophoneState {
//    NotAvailable = 4,
//    Enabled = 2,
//    Active = 1,
//    Disabled = 3,
//}



lazy_static! {
       static ref ENCRYPTION_KEYS_CONNECTION: RwLock<Vec<String>> = RwLock::new(Vec::new());
}


lazy_static! {
    static ref ADMIN_PASSWORD: RwLock<String> = RwLock::new(String::new());
}

static CHAT_MESSAGE_ID: AtomicUsize = AtomicUsize::new(0);
static ICON_ID: AtomicUsize = AtomicUsize::new(0);
static TAG_ID: AtomicUsize = AtomicUsize::new(0);


fn update_chat_message_id() {
    CHAT_MESSAGE_ID.fetch_add(1, Ordering::SeqCst);
}

fn update_icon_id() {
    ICON_ID.fetch_add(1, Ordering::SeqCst);
}

fn update_tag_id() {
    TAG_ID.fetch_add(1, Ordering::SeqCst);
}

fn send_for_clients_in_channel_maintainer_id(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, channel_id: u64, new_maintainer_id: u64, ) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("channel_maintainer_id"));
    json_message_object.insert(String::from("maintainer_id"),serde_json::Value::from(new_maintainer_id));
    json_message_object.insert(String::from("channel_id"),serde_json::Value::from(channel_id));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false{
            continue;
        }

        if client.channel_id != channel_id {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {

                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn is_public_key_present_in_client_stored_data(client_stored_data: &mut Vec<ClientStoredData>, clients_public_key: String) -> bool
{
    let mut result = false;

    for data in client_stored_data {

        if data.public_key == clients_public_key {
            result = true;
            break;
        }
    }

    return result;
}

fn get_client_stored_data_by_public_key(client_stored_data: &mut Vec<ClientStoredData>, clients_public_key: String) -> Option<&mut ClientStoredData> {
    let mut result: Option<&mut ClientStoredData> = Option::None;

    for data in client_stored_data {

        if data.public_key == clients_public_key {
            println!("found the public key..");
            result = Option::Some(data);
            break;
        }
    }

    return result;
}

fn is_tag_id_present_in_client_stored_data(client_stored_data: &mut Vec<ClientStoredData>, clients_public_key: String, tag_id: u64) -> bool
{
    let mut result = false;

    for data in client_stored_data {

        if data.public_key == clients_public_key {
            if data.tag_ids.contains(&tag_id) == true {
                result = true;
                break;
            }
        }
    }
    return result;
}

fn get_tag_ids_for_public_key_from_client_stored_data(client_stored_data: &mut Vec<ClientStoredData>, clients_public_key: String) -> Vec<u64>
{
    let mut result: Vec<u64> = Vec::new();

    for data in client_stored_data {

        if data.public_key == clients_public_key {
            result = data.tag_ids.clone();
        }
    }
    return result;
}

fn send_connection_check_response_to_single_cient(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, client_id: u64) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("connection_check_response"));
    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false{
            continue;
        }

        if client.client_id != client_id {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {

                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn send_image_sent_status_back_to_sender(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, client_id: u64, status: String) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("image_sent_status"));
    json_message_object.insert(String::from("value"),serde_json::Value::from(status));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false{
            continue;
        }

        if client.client_id != client_id {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {

                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn send_public_key_challenge_to_single_client(single_client: &mut Client, websockets: &HashMap<u64, Responder>, random_string: String) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("public_key_challenge"));
    json_message_object.insert(String::from("value"),serde_json::Value::from(random_string));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    if single_client.is_existing == false {
        return;
    }

    let current_client_websocket: Option<&Responder> = websockets.get(&single_client.client_id);

    match current_client_websocket {
        None => {}
        Some(websocket) => {

            println!("sending public key challenge to client {}", single_client.client_id);

            let json_root_object1: Map<String, Value> = json_root_object.clone();

            let test = serde_json::Value::Object(json_root_object1);
            let data_content: String = serde_json::to_string(&test).unwrap();
            let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
            websocket.send(Message::Text(data_to_send_base64));
        }
    }
}

fn send_maintainer_id_to_single_client(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>,  channel_id: u64,  client_id: u64, maintainer_id_to_send: u64, ) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("channel_maintainer_id"));
    json_message_object.insert(String::from("maintainer_id"),serde_json::Value::from(maintainer_id_to_send));
    json_message_object.insert(String::from("channel_id"),serde_json::Value::from(channel_id));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false{
            continue;
        }

        if client.client_id != client_id {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {

                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

//
//client needs to know what message id got assigned to his sent message
//

fn send_server_chat_message_id_for_local_message_id(websockets: &HashMap<u64, Responder>, client_id: u64, server_chat_message_id: usize, local_message_id: usize) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("server_chat_message_id_for_local_message_id"));
    json_message_object.insert(String::from("local_message_id"), serde_json::Value::from(local_message_id));
    json_message_object.insert(String::from("server_chat_message_id"), serde_json::Value::from(server_chat_message_id));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let current_client_websocket: Option<&Responder> = websockets.get(&client_id);

    match current_client_websocket {
        None => {}
        Some(websocket) => {

            let json_root_object1: Map<String, Value> = json_root_object.clone();

            let test = serde_json::Value::Object(json_root_object1);
            let data_content: String = serde_json::to_string(&test).unwrap();
            let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
            websocket.send(Message::Text(data_to_send_base64));
        }
    }
}

fn send_access_denied_to_single_client(websockets: &HashMap<u64, Responder>, client_id: u64,) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("access_denied"));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let current_client_websocket: Option<&Responder> = websockets.get(&client_id);

    match current_client_websocket {
        None => {}
        Some(websocket) => {

            let json_root_object1: Map<String, Value> = json_root_object.clone();

            let test = serde_json::Value::Object(json_root_object1);
            let data_content: String = serde_json::to_string(&test).unwrap();
            let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
            websocket.send(Message::Text(data_to_send_base64));
        }
    }
}

fn broadcast_peer_connection_state(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, client_id: u64, peer_connection_state: u64) {
    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("peer_connection_state_of_client"));
    json_message_object.insert(String::from("peer_connection_state"),serde_json::Value::from(peer_connection_state));
    json_message_object.insert(String::from("client_id"),serde_json::Value::from(client_id));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}



fn send_ice_candidate_to_single_client(websocket: &Responder, value: String) {
    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    let json_value_object: serde_json::Value = serde_json::from_str(value.as_str()).unwrap();

    json_message_object.insert(String::from("type"), serde_json::Value::from("ice_candidate"));
    json_message_object.insert(String::from("value"), serde_json::Value::from(json_value_object));
    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let test = serde_json::Value::Object(json_root_object);
    let data_content: String = serde_json::to_string(&test).unwrap();
    let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);

    websocket.send(Message::Text(data_to_send_base64));
}


fn send_active_microphone_usage_for_current_channel_to_single_client(clients: &mut HashMap<u64, Client>, responder: &Responder, current_channel_id: u64) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_clients_array: Vec<serde_json::Map<String, serde_json::Value>> = vec![];


    for (_key, client) in clients {

        if client.channel_id != current_channel_id {
            continue;
        }

        //have to send even to ourselves
        //if client.client_id == current_client_id {
        //    continue;
        //}

        //only active mics are relevant
        if client.microphone_state != 1 {
            continue;
        }


        let mut single_client_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
        single_client_object.insert(String::from("client_id"), serde_json::Value::from(client.client_id));
        single_client_object.insert(String::from("microphone_state"), serde_json::Value::from(client.microphone_state));
        single_client_object.insert(String::from("is_streaming_song"), serde_json::Value::from(client.is_streaming_song));
        single_client_object.insert(String::from("song_name"), serde_json::Value::from(client.song_name.clone()));

        json_clients_array.push(single_client_object);
    }

    if json_clients_array.is_empty() == true
    {
        return;
    }

    json_message_object.insert(String::from("type"), serde_json::Value::from("current_channel_active_microphone_usage"));
    json_message_object.insert(String::from("clients"), serde_json::Value::from(json_clients_array));
    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let test = serde_json::Value::Object(json_root_object);
    let data_content: String = serde_json::to_string(&test).unwrap();

    let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);

    responder.send(Message::Text(data_to_send_base64));
}

fn broadcast_microphone_usage(clients: &mut HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, client_id: u64, channel_id: u64, microphone_usage: u64) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("microphone_usage"));
    json_message_object.insert(String::from("client_id"),serde_json::Value::from(client_id));
    json_message_object.insert(String::from("channel_id"),serde_json::Value::from(channel_id));
    json_message_object.insert(String::from("value"),serde_json::Value::from(microphone_usage));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false{
            continue;
        }

        if microphone_usage == 1 {
            //if we want to send information about speaking (push to talk) only sent to clients in actual channel
            if client.channel_id != channel_id {
                continue;
            }
        }

        //if client.client_id == client_id {
        //    continue;
        //}

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {

                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn is_maintainer_of_channel_leaving_that_channel(channels: &mut HashMap<u64, Channel>, client_id_to_check: u64) -> (bool, u64) {
    let mut result: (bool, u64) = (false, 0);
    //chann

    for (_key, channel) in channels {

        if channel.is_channel == false {
            continue;
        }

        if channel.is_channel_maintainer_present == true {
            if channel.maintainer_id == client_id_to_check{
                result.0 = true;
                result.1 = channel.channel_id;
                break;
            }
        }
    }

    return result;
}

fn find_new_maintainer_for_channel(clients: &mut HashMap<u64, Client>, channels: &mut HashMap<u64, Channel>, id_of_client_that_disconnected: u64, channel_id_to_find: u64, mind_the_client_that_left_disconnected: bool) -> (bool, u64) {
    let mut result: (bool, u64) = (false, 0);

    //try to find new maintainer

    if mind_the_client_that_left_disconnected == true {
        for (_key, client) in clients {
            //if client has same channel_id as client that is leaving and not current_client
            if client.channel_id == channel_id_to_find && id_of_client_that_disconnected != client.client_id {
                result.0 = true;    //found the maintainer
                result.1 = client.client_id;  //client_id/client_id of new maintainer
                break;
            }
        }
    } else {
        for (_key, client) in clients {
            //if client has same channel_id as client that is leaving and not current_client
            if client.channel_id == channel_id_to_find {
                result.0 = true;    //found the maintainer
                result.1 = client.client_id;  //client_id/client_id of new maintainer
                break;
            }
        }
    }

    //if we found the maintainer
    if result.0 == true {
        for (_key, channel) in channels {
            if channel_id_to_find == channel.channel_id {
                channel.maintainer_id = result.1; //set maintainer id
                channel.is_channel_maintainer_present = true;
            }
        }
    } else {

        let aa = channels.get_mut(&channel_id_to_find);
        match aa {
            None => {
                println!("!!!!CANNOT SET is_channel_maintainer_present to false, CANNOT FIND CHANNEL!!!!");
            }
            Some(value) => {
                value.maintainer_id = 0;
                value.is_channel_maintainer_present = false;
                println!("setting is_channel_maintainer_present to false for channel {}" , value.name.clone());
            }
        }
    }

    return result;
}

fn send_cross_thread_message_client_disconnect(sender: &std::sync::mpsc::Sender<String>, client_id: u64) {
    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_root_object.insert(String::from("type"), serde_json::Value::from("client_disconnect"));
    json_root_object.insert(String::from("client_id"), serde_json::Value::from(client_id));

    let data_content: String = serde_json::to_string(&json_root_object).unwrap();

    sender.send(data_content).expect("");
}

fn send_cross_thread_message_create_new_client_at_rtc_thread(sender: &std::sync::mpsc::Sender<String>, client_id: u64) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_root_object.insert(String::from("type"), serde_json::Value::from("client_connect"));
    json_root_object.insert(String::from("client_id"), serde_json::Value::from(client_id));

    let data_content: String = serde_json::to_string(&json_root_object).unwrap();

    sender.send(data_content).expect("");
}

fn send_cross_thread_message_sdp_answer(sender: &std::sync::mpsc::Sender<String>, client_id: u64, value: serde_json::Value) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_root_object.insert(String::from("type"), serde_json::Value::from("sdp_answer"));
    json_root_object.insert(String::from("client_id"), serde_json::Value::from(client_id));
    json_root_object.insert(String::from("value"), value);

    let data_content: String = serde_json::to_string(&json_root_object).unwrap();

    sender.send(data_content).expect("TODO: panic message");
}

fn send_cross_thread_message_ice_candidate(sender: &std::sync::mpsc::Sender<String>, client_id: u64, value: serde_json::Value) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_root_object.insert(String::from("type"), serde_json::Value::from("ice_candidate_from_client"));
    json_root_object.insert(String::from("client_id"), serde_json::Value::from(client_id));
    json_root_object.insert(String::from("value"), value);

    let data_content: String = serde_json::to_string(&json_root_object).unwrap();

    sender.send(data_content).expect("send_cross_thread_message_ice_candidate xeept");
}

fn send_cross_thread_message_channel_join(sender: &std::sync::mpsc::Sender<String>, client_id: u64, channel_id: u64) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_root_object.insert(String::from("type"), serde_json::Value::from("channel_join"));
    json_root_object.insert(String::from("client_id"), serde_json::Value::from(client_id));
    json_root_object.insert(String::from("channel_id"), serde_json::Value::from(channel_id));

    let data_content: String = serde_json::to_string(&json_root_object).unwrap();

    sender.send(data_content).expect("");
}

fn broadcast_client_disconnect(clients: &mut HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, client_id_of_disconnected: u64)  {

    let client_option: Option<&Client> = clients.get(&client_id_of_disconnected);

    match client_option {
        None => {}
        Some(client_that_connected) => {

            let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
            let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

            json_message_object.insert(String::from("type"), serde_json::Value::from("client_disconnect"));
            json_message_object.insert(String::from("client_id"),serde_json::Value::from(client_that_connected.client_id));

            json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

            for (_key, client) in clients {

                if client.is_existing == false {
                    continue;
                }

                if client.is_authenticated == false{
                    continue;
                }

                if client.client_id == client_id_of_disconnected {
                    continue;
                }

                let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

                match current_client_websocket {
                    None => {}
                    Some(websocket) => {
                        let json_root_object1: Map<String, Value> = json_root_object.clone();
                        let test = serde_json::Value::Object(json_root_object1);
                        let data_content: String = serde_json::to_string(&test).unwrap();
                        let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
                        websocket.send(Message::Text(data_to_send_base64));
                    }
                }
            }
        }
    }
}


fn process_client_connect(clients: &HashMap<u64, Client>, channels: &mut HashMap<u64, Channel>, websockets: &HashMap<u64, Responder>, client_id_of_connected: u64) {

    broadcast_client_connect(clients, websockets, client_id_of_connected);

    let root_channel_id = 0;

    let client_count_in_channel = get_client_count_for_channel(clients, root_channel_id);
    if client_count_in_channel == 1 {
        let mut root_channel = channels.get_mut(&root_channel_id).unwrap();
        root_channel.maintainer_id = client_id_of_connected;
        root_channel.is_channel_maintainer_present = true;
        send_maintainer_id_to_single_client(clients, websockets, root_channel_id, client_id_of_connected, client_id_of_connected);

    } else {
        let root_channel: &Channel = channels.get(&root_channel_id).unwrap();
        send_maintainer_id_to_single_client(clients, websockets, root_channel_id, client_id_of_connected, root_channel.maintainer_id as u64);
    }
}

fn get_client_ids_in_channel(clients: &HashMap<u64, Client>, channel_id: u64) -> Vec<u64> {
    let mut result: Vec<u64> = vec![];

    for (_key, client) in clients {
        println!("client.channel_id {:?}",  client.channel_id);
        println!("channel_id {:?}",  channel_id);

        if client.channel_id == channel_id {
            result.push(client.client_id);
        }
    }

    return result;
}


fn broadcast_tag_add(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, new_tag: &Tag) {

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
        let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

        let tag_name: String = new_tag.name.clone();
        let icon_id: u64 = new_tag.icon_id.clone();
        let tag_id: u64 = new_tag.id.clone();

        json_message_object.insert(String::from("type"), serde_json::Value::from("tag_add"));
        json_message_object.insert(String::from("tag_id"),serde_json::Value::from(tag_id));
        json_message_object.insert(String::from("tag_name"),serde_json::Value::from(tag_name));
        json_message_object.insert(String::from("tag_linked_icon_id"),serde_json::Value::from(icon_id));

        json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();
                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}


fn broadcast_add_icon(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, new_icon: &Icon) {

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
        let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();


        let base64_icon: String = new_icon.base64_icon.clone();
        let icon_id: u64 = new_icon.id.clone();

        json_message_object.insert(String::from("type"), serde_json::Value::from("icon_add"));
        json_message_object.insert(String::from("icon_id"),serde_json::Value::from(icon_id));
        json_message_object.insert(String::from("base64_icon"),serde_json::Value::from(base64_icon));

        json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();
                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn broadcast_remove_tag_from_client(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, client_id: u64, tag_id: u64) {

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
        let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

        json_message_object.insert(String::from("type"), serde_json::Value::from("remove_tag_from_client"));
        json_message_object.insert(String::from("tag_id"),serde_json::Value::from(tag_id));
        json_message_object.insert(String::from("client_id"),serde_json::Value::from(client_id));

        json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}


fn broadcast_add_tag_to_client(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, client_id: u64, tag_id: u64) {

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
        let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

        json_message_object.insert(String::from("type"), serde_json::Value::from("tag_add_to_client"));
        json_message_object.insert(String::from("tag_id"),serde_json::Value::from(tag_id));
        json_message_object.insert(String::from("client_id"),serde_json::Value::from(client_id));

        json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn broadcast_channel_join(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, client_id: u64, new_channel_id: u64) {

    let client_joining_channel_microphone_state= clients.get(&client_id).unwrap().microphone_state;
    let client_joining_channel_is_streaming_song: bool = clients.get(&client_id).unwrap().is_streaming_song;
    let client_joining_channel_song_name: String = clients.get(&client_id).unwrap().song_name.clone();

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        //
        //if microphone is active, send microphone state as active only for clients of channel that client is joining
        //

        let mut microphone_state: u64 = client_joining_channel_microphone_state;
        let mut is_streaming_song: bool = client_joining_channel_is_streaming_song;
        let mut song_name: String = client_joining_channel_song_name.clone();

        //
        //clients that are not in the same channel do not need accurate microphone usage information, for privacy reasons
        //

        if client.channel_id != new_channel_id
        {
            if microphone_state == 1 {
                microphone_state = 2;
            }
            is_streaming_song = false;
            song_name = String::from("");
        }

        let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
        let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

        json_message_object.insert(String::from("type"), serde_json::Value::from("channel_join"));
        json_message_object.insert(String::from("channel_id"),serde_json::Value::from(new_channel_id));
        json_message_object.insert(String::from("client_id"),serde_json::Value::from(client_id));
        json_message_object.insert(String::from("microphone_state"),serde_json::Value::from(microphone_state));
        json_message_object.insert(String::from("is_streaming_song"), serde_json::Value::from(is_streaming_song));
        json_message_object.insert(String::from("song_name"),serde_json::Value::from(song_name));

        json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));


        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn broadcast_channel_edit(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, channel: &Channel) {
    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("channel_edit"));
    json_message_object.insert(String::from("channel_id"),serde_json::Value::from(channel.channel_id));
    json_message_object.insert(String::from("channel_name"),serde_json::Value::from(channel.name.clone()));
    json_message_object.insert(String::from("channel_description"),serde_json::Value::from(channel.description.clone()));
    json_message_object.insert(String::from("is_using_password"),serde_json::Value::from(channel.is_using_password.clone()));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {
        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn broadcast_channel_delete(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, channel_id: u64) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("channel_delete"));
    json_message_object.insert(String::from("channel_id"),serde_json::Value::from(channel_id));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {
        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn send_edit_chat_message_to_selected_clients(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>,client_ids: &Vec<u64>, chat_message_id: usize, new_message_value: String) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("chat_message_edit"));
    json_message_object.insert(String::from("chat_message_id"),serde_json::Value::from(chat_message_id));
    json_message_object.insert(String::from("new_message_value"),serde_json::Value::from(new_message_value));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {
        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        if client_ids.contains(&client.client_id) == false {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn send_delete_chat_message_to_selected_clients(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>,client_ids: &Vec<u64>, chat_message_id: usize) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("chat_message_delete"));
    json_message_object.insert(String::from("chat_message_id"),serde_json::Value::from(chat_message_id));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {
        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        if client_ids.contains(&client.client_id) == false {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn send_stop_song_stream_message_to_selected_clients(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>,client_ids: &Vec<u64>, client_id: u64) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("stop_song_stream"));
    json_message_object.insert(String::from("client_id"),serde_json::Value::from(client_id));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {
        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        if client_ids.contains(&client.client_id) == false {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn send_start_song_stream_message_to_selected_clients(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>,client_ids: &Vec<u64>, client_id: u64, song_name: String) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("start_song_stream"));
    json_message_object.insert(String::from("client_id"),serde_json::Value::from(client_id));
    json_message_object.insert(String::from("song_name"),serde_json::Value::from(song_name));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {
        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        if client_ids.contains(&client.client_id) == false {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64(data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn broadcast_client_connect(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, client_id: u64) {

    let client_option: Option<&Client> = clients.get(&client_id);

    match client_option {
        None => {}
        Some(client_that_connected) => {

            let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
            let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

            json_message_object.insert(String::from("type"), serde_json::Value::from("client_connect"));
            json_message_object.insert(String::from("username"), serde_json::Value::from(client_that_connected.username.as_str()));
            json_message_object.insert(String::from("public_key"),serde_json::Value::from(client_that_connected.public_key.as_str()));
            json_message_object.insert(String::from("channel_id"),serde_json::Value::from(client_that_connected.channel_id));
            json_message_object.insert(String::from("client_id"),serde_json::Value::from(client_that_connected.client_id));
            json_message_object.insert(String::from("tag_ids"), serde_json::Value::from(client_that_connected.tag_ids.clone()));

            json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

            for (_key, client) in clients {

                if client.is_existing == false {
                    continue;
                }

                if client.is_authenticated == false{
                    continue;
                }

                if client.client_id == client_id {
                    continue;
                }

                let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

                match current_client_websocket {
                    None => {}
                    Some(websocket) => {

                        let json_root_object1: Map<String, Value> = json_root_object.clone();

                        let test = serde_json::Value::Object(json_root_object1);
                        let data_content: String = serde_json::to_string(&test).unwrap();
                        let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
                        websocket.send(Message::Text(data_to_send_base64));
                    }
                }
            }
        }
    }
}

fn broadcast_client_username_change(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, client_id: u64) {

    let client_option: Option<&Client> = clients.get(&client_id);

    match client_option {
        None => {}
        Some(client_who_changed_its_name) => {

            let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
            let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

            json_message_object.insert(String::from("type"), serde_json::Value::from("client_rename"));
            json_message_object.insert(String::from("new_username"), serde_json::Value::from(client_who_changed_its_name.username.clone()));
            json_message_object.insert(String::from("client_id"), serde_json::Value::from(client_who_changed_its_name.client_id));

            json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

            for (_key, client) in clients {

                if client.is_existing == false {
                    continue;
                }

                if client.is_authenticated == false {
                    continue;
                }

                let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

                match current_client_websocket {
                    None => {}
                    Some(websocket) => {

                        let json_root_object1: Map<String, Value> = json_root_object.clone();

                        let test = serde_json::Value::Object(json_root_object1);
                        let data_content: String = serde_json::to_string(&test).unwrap();

                        println!("data_content {}", &data_content);

                        let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
                        websocket.send(Message::Text(data_to_send_base64));
                    }
                }
            }
        }
    }
}

fn send_client_list_to_single_client(clients: &mut HashMap<u64, Client>, responder: &Responder, current_client_username: String) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_clients_array: Vec<serde_json::Map<String, serde_json::Value>> = vec![];

    for (_key, client) in clients {
        let mut single_client_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
        single_client_object.insert(String::from("username"), serde_json::Value::from(client.username.as_str()));
        single_client_object.insert(String::from("public_key"), serde_json::Value::from(client.public_key.as_str()));
        single_client_object.insert(String::from("channel_id"), serde_json::Value::from(client.channel_id));
        single_client_object.insert(String::from("client_id"), serde_json::Value::from(client.client_id));

        let mut microphone_state: u64 = client.microphone_state;
        if microphone_state == 1 {
            microphone_state = 2;
        }
        single_client_object.insert(String::from("microphone_state"), serde_json::Value::from(microphone_state));

        single_client_object.insert(String::from("tag_ids"), serde_json::Value::from(client.tag_ids.clone()));
        json_clients_array.push(single_client_object);
    }

    json_message_object.insert(String::from("type"), serde_json::Value::from("client_list"));
    json_message_object.insert(String::from("clients"), serde_json::Value::from(json_clients_array));
    json_message_object.insert(String::from("local_username"),serde_json::Value::from(current_client_username.as_str()));
    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let test = serde_json::Value::Object(json_root_object);
    let data_content: String = serde_json::to_string(&test).unwrap();

    let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);

    responder.send(Message::Text(data_to_send_base64));
}

fn check_if_username_change_allowed(client_id: u64,  clients: &mut HashMap<u64, Client>,  message: &serde_json::Value) -> bool {

    let client_that_wants_to_change_username: &Client = clients.get(&client_id).unwrap();

    let datetime: chrono::DateTime<chrono::Utc> = chrono::Utc::now();
    let timestamp_now: i64 = datetime.timestamp();

    if client_that_wants_to_change_username.timestamp_username_changed + 3 > timestamp_now {
        return false;
    }

    let new_desired_username: &String = &message["message"]["new_username"].to_string();

    if new_desired_username.is_empty() == true {
        return false;
    }

    if new_desired_username.len() > 50 {
        return false;
    }

    for (_key, client) in clients {
        if client.username == new_desired_username.to_owned() {
            return false;
        }
    }

    return true;
}

fn is_channel_create_allowed(client_that_creates_channel: &mut Client, channels: &mut HashMap<u64, Channel>, message: &serde_json::Value) -> bool {
    let mut result: bool = true;

    //check cooldown
    let datetime: chrono::DateTime<chrono::Utc> = chrono::Utc::now();
    let timestamp_now: i64 = datetime.timestamp();

    if client_that_creates_channel.timestamp_last_channel_created + 1> timestamp_now {
        result = false;
    }

    //first check if json fields exist

    if message["message"]["parent_channel_id"] == false {
        println!("field message.parent_channel_id does not exist");
        result = false;
    }

    if message["message"]["channel_name"] == false  {
        println!("field message.channel_name does not exist");
        result = false;
    }

    if message["message"]["channel_description"] == false {
        println!("field message.channel_description does not exist");
        result = false;
    }

    if message["message"]["channel_password"] == false {
        println!("field message.channel_password does not exist");
        result = false;
    }


    if message["message"]["parent_channel_id"].is_i64() == false {
        println!("field message.parent_channel_id wrong type");
        result = false;
    }

    if message["message"]["channel_name"].is_string() == false {
        println!("field message.channel_name wrong type");
        result = false;
    }

    if message["message"]["channel_description"].is_string() == false {
        println!("field message.channel_description wrong type");
        result = false;
    }

    if message["message"]["channel_password"].is_string() == false {
        println!("field message.channel_password wrong type");
        result = false;
    }

    if result == true {
        let msg_parent_channel_id = message["message"]["parent_channel_id"].as_i64().unwrap();
        let msg_channel_name = String::from(message["message"]["channel_name"].as_str().unwrap());
        let msg_channel_description = String::from(message["message"]["channel_description"].as_str().unwrap());
        let msg_channel_password = String::from(message["message"]["channel_password"].as_str().unwrap());


        //if result still is true, check if parent channel exists
        if result == true {
            result = false;
            for (_key, channel) in channels.iter() {

                if msg_parent_channel_id == -1 {
                    continue;
                }

                if channel.channel_id == msg_parent_channel_id as u64{
                    result = true;
                    break;
                }
            }
        }

        if msg_channel_name.len() > 50 {
            result = false;
        }

        if msg_channel_description.len() > 1000 {
            result = false;
        }

        if msg_channel_password.len() > 30 {
            result = false;
        }
    }

    client_that_creates_channel.timestamp_last_channel_created = timestamp_now;

    return result;
}

fn broadcast_channel_create(clients: &mut HashMap<u64, Client>, created_channel: &Channel, websockets: &mut HashMap<u64, Responder>) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("channel_create"));
    json_message_object.insert(String::from("channel_id"), serde_json::Value::from(created_channel.channel_id));
    json_message_object.insert(String::from("parent_channel_id"), serde_json::Value::from(created_channel.parent_channel_id));
    json_message_object.insert(String::from("name"), serde_json::Value::from(created_channel.name.clone()));
    json_message_object.insert(String::from("description"), serde_json::Value::from(created_channel.description.clone()));
    json_message_object.insert(String::from("maintainer_id"), serde_json::Value::from(-1));
    json_message_object.insert(String::from("is_using_password"), serde_json::Value::from(created_channel.is_using_password));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {
                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();

                let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn is_sdp_answer_message_valid(message: &serde_json::Value) -> bool {
    let mut result: bool = true;

    if message["message"]["type"] == false {
        println!("field message.type does not exist");
        result = false;
    }

    if message["message"]["value"] == false {
        println!("field message.value does not exist");
        result = false;
    }

    if message["message"]["type"].is_string() == false {
        result = false;
    }

    if message["message"]["value"].is_object() == false {
        result = false;
    }


    if result == true {

        if message["message"]["value"]["sdp"] == false {
            println!("message[value][sdp] == false");
            result = false;
        }

        if message["message"]["value"]["type"] == false {
            println!("message[value][type] == false");
            result = false;
        }

        if message["message"]["value"]["sdp"].is_string() == false {
            println!("message[value][sdp].is_string() == false");
            result = false;
        }


        if message["message"]["value"]["type"].is_string() == false {
            println!("message[value][type].is_string() == false");
            result = false;
        }
    }

    return result;
}

fn is_ice_candidate_message_valid(message: &serde_json::Value) -> bool {
    let mut result: bool = true;

    if message["message"]["type"] == false {
        println!("field message.type does not exist");
        result = false;
        return result;
    }

    if message["message"]["value"] == false {
        println!("field message.value does not exist");
        result = false;
        return result;
    }

    if message["message"]["type"].is_string() == false {
        println!("message[message][type.is_string()");
        result = false;
        return result;
    }

    if message["message"]["value"].is_object() == false {
        println!("message[message][value].is_object()");
        result = false;
        return result;
    }

    if result == true {

        if message["message"]["value"]["candidate"] == false {
            println!("message[value][candidate] == false");
            result = false;
        }

        if message["message"]["value"]["sdpMid"] == false {
            println!("message[value][sdpMid] == false");
            result = false;
        }

        if message["message"]["value"]["sdpMLineIndex"] == false {
            println!("message[value][sdpMLineIndex] == false");
            result = false;
        }

        if message["message"]["value"]["candidate"].is_string() == false {
            println!("message[value][candidate].is_string() == false");
            result = false;
        }

        if message["message"]["value"]["sdpMid"].is_string() == false {
            println!("message[value][sdpMid].is_string() == false");
            result = false;
        }

        if message["message"]["value"]["sdpMLineIndex"].is_u64() == false {
            println!("message[value][sdpMLineIndex].is_u64() == false");
            result = false;
        }
    }

    return result;
}

fn is_channel_join_message_valid(clients: &mut HashMap<u64, Client>, client_id: u64, message: &serde_json::Value) -> bool {

    let mut result: bool = true;

    let datetime: chrono::DateTime<chrono::Utc> = chrono::Utc::now();
    let timestamp_now: i64 = datetime.timestamp();

    let mut client = clients.get_mut(&client_id).unwrap();

    if client.timestamp_last_sent_join_channel_request + 1 > timestamp_now {
        result = false;
    }

    if message["message"]["type"] == false {
        println!("field message.type does not exist");
        result = false;
        return result;
    }

    if message["message"]["channel_id"] == false {
        println!("field message.channel_id does not exist");
        result = false;
        return result;
    }

    if message["message"]["channel_password"] == false {
        println!("field message.channel_password does not exist");
        result = false;
        return result;
    }

    if message["message"]["type"].is_string() == false {
        println!("type ");
        result = false;
        return result;
    }

    if message["message"]["channel_id"].is_i64() == false {
        println!("channel_id");
        result = false;
        return result;
    }

    if message["message"]["channel_password"].is_string() == false {
        println!("channel_password");
        result = false;
        return result;
    }

    if result == true {

        client.timestamp_last_sent_join_channel_request = timestamp_now;
    }

    return result;
}

fn is_edit_chat_message_request_valid(message: &serde_json::Value) -> bool {

    let mut result: bool = true;

    if message["message"]["message_id"].is_i64() == false {
        result = false;
        return result;
    }


    //if everything is still allright check if user is trying to delete root channel
    if message["message"]["message_id"].as_i64().unwrap() == 0 {
        result = false;
        return result;
    }


    if message["message"]["new_message_value"] == false {
        println!("field message.new_message_value does not exist");
        result = false;
        return result;
    }

    if message["message"]["new_message_value"].is_string() == false {
        println!("new_message_value isnt string");

        result = false;
        return result;
    }

    return result;
}

fn is_start_song_stream_message_valid(message: &serde_json::Value) -> bool {

    let mut result: bool = true;

    if message["message"]["song_name"] == false {
        println!("field message.song_name does not exist");
        result = false;
    }

    if message["message"]["song_name"].is_string() == false {
        result = false;
    }

    return result;
}

fn print_out_all_connected_clients(clients: &mut HashMap<u64, Client>)  {

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false{
            continue;
        }

        println!("client {} {}", client.client_id, client.public_key);
    }
}

fn is_there_a_client_with_same_public_key(clients: &mut HashMap<u64, Client>, public_key: String) -> bool {

    let mut result = false;

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false{
            continue;
        }

        if client.public_key == public_key {
            result = true;
            break;
        }
    }

    return result;
}


fn is_delete_chat_message_request_valid(message: &serde_json::Value) -> bool {

    let mut result: bool = true;

    if message["message"]["message_id"].is_i64() == false {
        result = false;
    }

    if result == true {
        //if everything is still allright check if user is trying to delete root channel
        if message["message"]["message_id"].as_i64().unwrap() == 0 {
            result = false;
        }
    }

    return result;
}

fn is_channel_delete_request_valid(_clients: &HashMap<u64, Client>, message: &serde_json::Value) -> bool {

    let mut result: bool = true;

    if message["message"]["type"] == false {
        println!("field message.type does not exist");
        result = false;
    }

    if message["message"]["channel_id"] == false {
        println!("field message.channel_id does not exist");
        result = false;
    }

    if message["message"]["type"].is_string() == false {
        result = false;
    }

    if message["message"]["channel_id"].is_i64() == false {
        result = false;
    }

    if result == true {
        //if everything is still allright check if user is trying to delete root channel
        if message["message"]["channel_id"].as_i64().unwrap() == 0 {
            result = false;
        }
    }

    return result;
}

fn does_channel_exist(channels: &HashMap<u64, Channel>, channel_id: u64) -> bool {

    let mut result: bool = false;

    for (_key, channel) in channels.iter() {
        if channel.channel_id == channel_id {
            result = true;
            break;
        }
    }
    return result;
}

fn delete_channels_by_their_ids(channels: &mut HashMap<u64, Channel>, channel_ids: &Vec<u64>) {
    for channel_id in channel_ids {
        //channel id is same as its key in hashmap
        channels.remove(channel_id);
    }
}

fn find_sub_channels_to_delete(channels: &HashMap<u64, Channel>, channel_id_to_delete: u64) -> Vec<u64> {

    let mut result: Vec<u64> = vec![];

    for (_key, channel) in channels.iter() {
        if channel.parent_channel_id == channel_id_to_delete as i64 {
            result.push(channel.channel_id);
            let mut result1: Vec<u64>  = find_sub_channels_to_delete(channels, channel.channel_id);
            result.append(&mut result1);
        }
    }

    return result;
}

//for finding new maintainer
fn process_channel_delete_continue(clients: &mut HashMap<u64, Client>, channels: &mut HashMap<u64, Channel>, websockets: &mut HashMap<u64, Responder>) {

    let status2: (bool, u64) = find_new_maintainer_for_channel(clients, channels, 0, 0, false);

    let is_maintainer_found = status2.0;
    let new_maintainer_id = status2.1;

    //set new channel_id to client that wants to switch channel
    if is_maintainer_found == true {

        let username_new_maintainer = clients.get(&new_maintainer_id).unwrap().username.clone();

        println!("process_channel_delete2 found new maintainer for channel root. username: {}", username_new_maintainer);

        println!("process_channel_delete2 sending id of new maintainer for clients in channel root username: {}", username_new_maintainer);

        send_for_clients_in_channel_maintainer_id(clients, websockets, 0, new_maintainer_id);
        println!("process_channel_delete2 send_channel_maintainer_id");
    }
}

fn process_channel_delete(clients: &mut HashMap<u64, Client>, channels: &mut HashMap<u64, Channel>, websockets: &mut HashMap<u64, Responder>, message: &serde_json::Value) -> i32 {

    let mut result: i32 = 0;    //result = need_to_find_maintainer_for_root
    let status: bool = is_channel_delete_request_valid(clients, message);
    //value o -> there were no clients found in channel or subchennels to be moved.
    //value 1 -> there were clients found, moved to root channel, nobody in root channel
    //value 2 -> there were clients found, moved to root channel, somebody already in root

    if status == true {

        let root_channel_id: u64 = 0;
        let root_channel = channels.get(&root_channel_id).unwrap();

        let channel_id_for_deletion: u64 = message["message"]["channel_id"].as_u64().unwrap();

        let status1: bool = does_channel_exist(channels, channel_id_for_deletion);

        if status1 == true {
            let mut channels_to_delete: Vec<u64> = find_sub_channels_to_delete(channels, channel_id_for_deletion);
            channels_to_delete.push(channel_id_for_deletion);

            //broadcast moving of clients first

            for channel_id in channels_to_delete.clone() {

                let client_ids_in_channel: Vec<u64> = get_client_ids_in_channel(clients, channel_id);

                println!("client_ids_in_channel {:?}",  client_ids_in_channel.len());

                for client_id1 in client_ids_in_channel {

                    if root_channel.is_channel_maintainer_present == false {
                        result = 1; //there is no maintainer in root channel, find new one
                        let some_client: &mut Client = clients.get_mut(&client_id1).unwrap();
                        some_client.channel_id = 0; //id of root channel, where client should be moved
                        println!("sending broadcast_channel_join to {:?}",  client_id1);
                        broadcast_channel_join(clients, websockets, client_id1, 0);
                    } else {
                        println!("THERE IS A MAINTAINER IN ROOT");
                        //there is maintainer in root channel, broadcast_channel_join then send maintainer info
                        //we dont have to find new maintainer like in case where channel maintainer is present
                        //delete_channel_continue takes care of that case

                        let some_client: &mut Client = clients.get_mut(&client_id1).unwrap();
                        some_client.channel_id = 0; //id of root channel, where client should be moved
                        println!("sending broadcast_channel_join to {:?}",  client_id1);
                        broadcast_channel_join(clients, websockets, client_id1, 0);

                        let root_channel_id: u64 = 0;
                        let maintainer_of_root = channels.get(&root_channel_id).unwrap().maintainer_id;

                        send_maintainer_id_to_single_client(clients, websockets, 0, client_id1, maintainer_of_root);

                        result = 2;
                    }

                    //
                    //clients are moved to root channel when their current channel is deleted (thats why channel id 0 is used)
                    //after they are moved, what needs to be done is, sending of active state of users microphone in root channel

                    //

                    let websocket: &Responder = websockets.get(&client_id1).unwrap();
                    send_active_microphone_usage_for_current_channel_to_single_client(clients, websocket, 0);
                }
            }

            //then delete channels and broadcast channels delete

            delete_channels_by_their_ids(channels, &channels_to_delete);

            for channel_id in channels_to_delete {
                broadcast_channel_delete(clients, websockets, channel_id);
            }
        } else {
            println!("channel id not found");
        }
    }
    return result;
}

fn process_edit_chat_message_request(clients: &mut HashMap<u64, Client>, websockets: &mut HashMap<u64, Responder>, message: &serde_json::Value, client_id: u64) {

    let result: bool = is_edit_chat_message_request_valid(message);

    if result == true {

        println!("is_edit_chat_message_request_valid == true");
        let message_id_to_edit: usize = message["message"]["message_id"].as_i64().unwrap() as usize;
        let new_message_value: String = message["message"]["new_message_value"].as_str().unwrap().to_string();

        let client_option: Option<&mut Client> = clients.get_mut(&client_id);

        match client_option {
            None => {}
            Some(client) => {

                let mut is_client_owner_of_message: bool = false;
                let mut detected_message_type: i8 = 0;
                let mut detected_receiver_id: u64 = 0;

                for entry in client.message_ids.iter() {
                    if entry.message_id == message_id_to_edit {
                        is_client_owner_of_message = true;
                        detected_message_type = entry.message_type;
                        detected_receiver_id = entry.receiver_id;
                        break;
                    }
                }

                if is_client_owner_of_message == true {

                    if detected_message_type == 1 {
                        println!("trying to edit channel chat message");
                        let mut clients_ids: Vec<u64> = Vec::new();

                        //loop throuh clients, add those that have same channel_id as receiver_id
                        for entry in clients.iter() {
                            if entry.1.channel_id == detected_receiver_id {
                                clients_ids.push(entry.1.client_id);
                            }
                        }

                        send_edit_chat_message_to_selected_clients(clients, websockets, &clients_ids, message_id_to_edit, new_message_value);

                    } else if detected_message_type == 2 {
                        println!("trying to delete private chat message");
                        let mut clients_ids: Vec<u64> = Vec::new();
                        clients_ids.push(detected_receiver_id); //append both receiver and sender
                        clients_ids.push(client.client_id);
                        send_edit_chat_message_to_selected_clients(clients, websockets, &clients_ids, message_id_to_edit, new_message_value);
                    }
                }
            }
        }
    }
}

fn process_delete_chat_message_request(clients: &mut HashMap<u64, Client>, websockets: &mut HashMap<u64, Responder>, message: &serde_json::Value, client_id: u64) {

    let result: bool = is_delete_chat_message_request_valid(message);

    if result == true {

        let message_id_to_delete: usize = message["message"]["message_id"].as_i64().unwrap() as usize;

        let client_option: Option<&mut Client> = clients.get_mut(&client_id);

        match client_option {
            None => {}
            Some(client) => {

                let mut is_client_owner_of_message: bool = false;
                let mut detected_message_type: i8 = 0;
                let mut detected_index: usize = 0;
                let mut detected_receiver_id: u64 = 0;

                for entry in client.message_ids.iter() {
                    if entry.message_id == message_id_to_delete {
                        is_client_owner_of_message = true;
                        detected_message_type = entry.message_type;
                        detected_receiver_id = entry.receiver_id;
                        break;
                    }
                    detected_index += 1;
                }

                if is_client_owner_of_message == true {
                    client.message_ids.remove(detected_index);

                    println!("message deleted from Vec, index: {} ", detected_index);

                    if detected_message_type == 1 {
                        println!("trying to delete channel chat message");
                        let mut clients_ids: Vec<u64> = Vec::new();

                        //loop throuh clients, add those that have same channel_id as receiver_id
                        for entry in clients.iter() {
                            if entry.1.channel_id == detected_receiver_id {
                                clients_ids.push(entry.1.client_id);
                            }
                        }

                        send_delete_chat_message_to_selected_clients(clients, websockets, &clients_ids, message_id_to_delete);

                    } else if detected_message_type == 2 {
                        println!("trying to delete private chat message");
                        let mut clients_ids: Vec<u64> = Vec::new();
                        clients_ids.push(detected_receiver_id); //append both receiver and sender
                        clients_ids.push(client.client_id);
                        send_delete_chat_message_to_selected_clients(clients, websockets, &clients_ids, message_id_to_delete);
                    }
                }
            }
        }
    }
}



fn is_public_key_challenge_response_valid(message: &serde_json::Value) -> bool {

    let mut result: bool = true;

    if message["message"]["value"] == false {
        println!("is_public_key_challenge_response_valid message.tag_id does not exist");
        result = false;
        return result;
    }

    if message["message"]["value"] == false {
        println!("is_public_key_challenge_response_valid message.client_id does not exist");
        result = false;
        return result;
    }

    return result;
}

fn is_public_key_info_message_valid(message: &serde_json::Value) -> bool {
    let mut result: bool = true;

    if message["message"]["value"] == false {
        println!("is_public_key_info_message_valid message.tag_id does not exist");
        result = false;
        return result;
    }

    if message["message"]["verification_string"] == false {
        println!("is_public_key_info_message_valid message.client_id does not exist");
        result = false;
        return result;
    }

    if message["message"]["value"].is_string() == false {
        println!("is_public_key_info_message_valid message.tag_id is not is_string");
        result = false;
        return result;
    }

    if message["message"]["verification_string"].is_string() == false {
        println!("is_public_key_info_message_valid  message.client_id is not is_string");
        result = false;
        return result;
    }

    return result;
}

fn is_add_remove_client_tag_message_valid(message: &serde_json::Value) -> bool {
    let mut result: bool = true;

    if message["message"]["tag_id"] == false {
        println!("is_add_remove_client_tag_message_valid field message.tag_id does not exist");
        result = false;
        return result;
    }

    if message["message"]["client_id"] == false {
        println!("is_add_remove_client_tag_message_valid field message.client_id does not exist");
        result = false;
        return result;
    }

    if message["message"]["tag_id"].is_u64() == false {
        println!("field message.tag_id is not u64");
        result = false;
        return result;
    }

    if message["message"]["client_id"].is_u64() == false {
        println!("field message.client_id is not u64");
        result = false;
        return result;
    }

    return result;
}

fn is_server_settings_add_new_tag_valid(message: &serde_json::Value) -> bool {

    let mut result: bool = true;

    if message["message"]["linked_icon_id"] == false {
        println!("field message.channel_id does not exist");
        result = false;
        return result;
    }

    if message["message"]["linked_icon_id"].is_i64() == false {
        println!("field message.linked_icon_id is not i64");

        result = false;
        return result;
    }


    if message["message"]["tag_name"] == false {
        println!("field message.tag_name does not exist");
        result = false;
        return result;
    }

    if message["message"]["tag_name"].is_string() == false {
        println!("tag_name isnt string");
        result = false;
        return result;
    }

    let tag_name: String = message["message"]["tag_name"].as_str().unwrap().to_string();

    if tag_name.len() > 20 || tag_name.len() < 1 {
        println!("tag_name length is not allowed {} ", tag_name.len());
        result = false;
        return result;
    }

    return result;
}

fn is_icon_upload_message_valid(message: &serde_json::Value) -> bool {

    let mut result: bool = true;

    if message["message"]["base64_icon_value"] == false {
        println!("field message.base64_icon_value does not exist");
        result = false;
        return result;
    }

    if message["message"]["base64_icon_value"].is_string() == false {
        println!("base64_icon_value isnt string");
        result = false;
        return result;

    }

    let base64string: String = message["message"]["base64_icon_value"].as_str().unwrap().to_string();

    if base64string.len() > 6650 {
        println!("base64_icon_value is too big");
        result = false;
        return result;
    }

    return result;
}


fn process_remove_tag_from_client(client_stored_data: &mut Vec<ClientStoredData>, clients: &mut HashMap<u64, Client>, websockets: &mut HashMap<u64, Responder>, tags: &mut HashMap<u64, Tag>, message: &serde_json::Value, client_id: u64) {

    let is_valid: bool = is_add_remove_client_tag_message_valid(&message);

    if is_valid == false
    {
        return;
    }

    let is_admin: bool = is_client_admin(clients, client_id);

    if is_admin == false {
        println!("client is not admin");
        return;
    }

    let client_id_to_remove_tag_from: u64 = message["message"]["client_id"].as_u64().unwrap();
    let tag_id: u64 = message["message"]["tag_id"].as_u64().unwrap();

    let client_option: Option<&mut Client> = clients.get_mut(&client_id_to_remove_tag_from);

    match client_option {
        None => {}
        Some(client) => {
            let tag_option: Option<&mut Tag> = tags.get_mut(&tag_id);

            match tag_option {
                None => {}
                Some(tag) => {

                    //
                    //if client under specified clientid exists and tag under specified tag id exists
                    //add tag id to vec if its not already there
                    //

                    if client.tag_ids.contains(&tag_id) == false {
                        println!("no tag to delete");
                        return;
                    }

                    client.tag_ids.retain(|&x| x != tag_id);

                    //
                    //if trying to remove admin tag, the one with ID: 0, set is_admin to false
                    //

                    if tag_id == 0 {
                        client.is_admin = false;
                    }

                    let status: bool = is_public_key_present_in_client_stored_data(client_stored_data, client.public_key.clone());

                    if status == false {

                        println!("public key {} not present, that should not be possible. If its really not present, there is nothing to do" , client.public_key.clone());

                    } else {
                        println!("public key {} present " , client.public_key.clone());

                        let status1: bool = is_tag_id_present_in_client_stored_data(client_stored_data, client.public_key.clone(), tag_id);
                        if status1 == true {

                            println!("is_tag_id_present_in_client_stored_data == false");

                            //
                            //there is ClientStoredData entry with clients public key, but does not contain tag id
                            //add tag id to it
                            //

                            let option_clientstoreddata = get_client_stored_data_by_public_key(client_stored_data, client.public_key.clone());

                            match option_clientstoreddata {
                                None => {}
                                Some(value) => {
                                    value.tag_ids.retain(|&x| x != tag_id);
                                    println!("removed tag {}", tag_id);
                                }
                            }
                        }
                    }
                    broadcast_remove_tag_from_client(clients, websockets, client_id_to_remove_tag_from, tag_id);
                }
            }
        }
    }
}


fn process_add_tag_to_client(client_stored_data: &mut Vec<ClientStoredData>, clients: &mut HashMap<u64, Client>, websockets: &mut HashMap<u64, Responder>, tags: &mut HashMap<u64, Tag>, message: &serde_json::Value, client_id: u64) {

    let is_valid: bool = is_add_remove_client_tag_message_valid(&message);

    if is_valid == false
    {
        return;
    }

    let is_admin: bool = is_client_admin(clients, client_id);

    if is_admin == false {
        println!("client is not admin");
        return;
    }

    let client_id_to_add_tag_to: u64 = message["message"]["client_id"].as_u64().unwrap();
    let tag_id: u64 = message["message"]["tag_id"].as_u64().unwrap();

    let client_option: Option<&mut Client> = clients.get_mut(&client_id_to_add_tag_to);

    match client_option {
        None => {}
        Some(client) => {
            let tag_option: Option<&mut Tag> = tags.get_mut(&tag_id);

            match tag_option {
                None => {}
                Some(tag) => {

                    //
                    //if client under specified clientid exists and tag under specified tag id exists
                    //add tag id to vec if its not already there
                    //

                    if client.tag_ids.contains(&tag_id) == true {
                        return;
                    }

                    client.tag_ids.push(tag_id);

                    //
                    //if trying to add admin tag, the one with ID: 0, set is_admin to true
                    //

                    if tag_id == 0 {
                        client.is_admin = true;
                    }

                    let status: bool = is_public_key_present_in_client_stored_data(client_stored_data, client.public_key.clone());

                    if status == false {

                        println!("public key {} not present adding public key" , client.public_key.clone());

                        let mut single_client_stored_data: ClientStoredData = ClientStoredData {
                            public_key: "".to_string(),
                            tag_ids: vec![],
                        };

                        single_client_stored_data.public_key = client.public_key.clone();
                        single_client_stored_data.tag_ids = Vec::new();
                        single_client_stored_data.tag_ids.push(tag_id);

                        client_stored_data.push(single_client_stored_data);

                        println!("public key added to client_stored_data");

                    } else {
                        println!("public key {} present " , client.public_key.clone());

                        let status1: bool = is_tag_id_present_in_client_stored_data(client_stored_data, client.public_key.clone(), tag_id);
                        if status1 == false {

                            println!("is_tag_id_present_in_client_stored_data == false");

                            //
                            //there is ClientStoredData entry with clients public key, but does not contain tag id
                            //add tag id to it
                            //

                            let option_clientstoreddata = get_client_stored_data_by_public_key(client_stored_data, client.public_key.clone());

                            match option_clientstoreddata {
                                None => {}
                                Some(value) => {
                                    value.tag_ids.push(tag_id);
                                    println!("added tag {}", tag_id);
                                }
                            }
                        }
                    }
                    broadcast_add_tag_to_client(clients, websockets, client_id_to_add_tag_to, tag_id);

                }
            }
        }
    }
}



fn process_server_settings_add_new_tag_message(clients: &mut HashMap<u64, Client>, websockets: &mut HashMap<u64, Responder>, tags: &mut HashMap<u64, Tag>, message: &serde_json::Value, client_id: u64) {

    let is_valid: bool = is_server_settings_add_new_tag_valid(&message);

    if is_valid == false
    {
        return;
    }

    println!("server_settings_add_new_tag valid");

    let client: &Client = clients.get(&client_id).unwrap();

    if client.is_admin == false
    {
        return;
    }


    let tag_id: u64 = TAG_ID.load(Ordering::SeqCst) as u64;
    let linked_icon_id: u64 = message["message"]["linked_icon_id"].as_u64().unwrap();
    let tag_name: String = message["message"]["tag_name"].as_str().unwrap().to_string();


    for (_key, tag) in tags.iter() {
        if tag.name == tag_name {
            return;
        }
    }

    let new_tag: Tag = Tag {
        id: tag_id,
        icon_id: linked_icon_id,
        name: tag_name.clone(),
    };

    tags.insert(tag_id, new_tag);

    let new_tag1: Tag = Tag {
        id: tag_id.clone(),
        icon_id: linked_icon_id.clone(),
        name: tag_name.clone(),
    };

    broadcast_tag_add(clients, websockets, &new_tag1);
    update_tag_id();
}

fn process_server_settings_icon_upload_message(clients: &mut HashMap<u64, Client>, websockets: &mut HashMap<u64, Responder>, icons: &mut HashMap<u64, Icon>, message: &serde_json::Value, client_id: u64) {

    let is_valid: bool = is_icon_upload_message_valid(&message);

    if is_valid == true {
        println!("icon_upload_message valid");

        let client: &Client = clients.get(&client_id).unwrap();

        if client.is_admin == true {

            let icon_id: u64 = ICON_ID.load(Ordering::SeqCst) as u64;
            let base64string: String = message["message"]["base64_icon_value"].as_str().unwrap().to_string();

            let new_icon: Icon = Icon {
                id: icon_id,
                base64_icon: base64string.clone(),
            };

            icons.insert(icon_id, new_icon);

            let new_icon1: Icon = Icon {
                id: icon_id,
                base64_icon: base64string.clone(),
            };

            broadcast_add_icon(clients, websockets, &new_icon1);

            update_icon_id();
        }


    } else {
        println!("message isnt valid");
    }
}


fn process_stop_song_stream_message(clients: &mut HashMap<u64, Client>, websockets: &mut HashMap<u64, Responder>, _message: &serde_json::Value, client_id: u64) {

    //
    //this message does not need to be checked for validity
    //json message object only contain "type" property which is already verified as valid if code got to this point
    //

    let client_option: Option<&mut Client> = clients.get_mut(&client_id);

    match client_option {
        None => {}
        Some(client) => {
            client.is_streaming_song = false;
            //client.mi

            let channel_id = client.channel_id.clone();

            let clients_ids: Vec<u64> = get_client_ids_in_channel(&clients, channel_id);

            send_stop_song_stream_message_to_selected_clients(clients, websockets, &clients_ids, client_id);
        }
    }

}

fn process_start_song_stream_message(clients: &mut HashMap<u64, Client>, websockets: &mut HashMap<u64, Responder>, message: &serde_json::Value, client_id: u64) {


    println!("process_start_song_stream_message");
    let result: bool = is_start_song_stream_message_valid(message);

    if result == true {

        println!("is_start_song_stream_message_valid == true");

        //
        //should playing multiple songs in channel be allowed?
        //

        let song_name: String = message["message"]["song_name"].as_str().unwrap().to_string();

        let client_option: Option<&mut Client> = clients.get_mut(&client_id);

        match client_option {
            None => {}
            Some(client) => {
                client.song_name = song_name.clone();
                client.is_streaming_song = true;

                let channel_id = client.channel_id.clone();

                let clients_ids: Vec<u64> = get_client_ids_in_channel(&clients, channel_id);

                send_start_song_stream_message_to_selected_clients(clients, websockets, &clients_ids, client_id, song_name);
            }
        }
    }
}


fn process_channel_create(clients: &mut HashMap<u64, Client>, channels: &mut HashMap<u64, Channel>, _websockets: &mut HashMap<u64, Responder>, message: &serde_json::Value, client_id: u64)  -> (bool, u64) {

    let mut result: (bool,  u64) = (false, 0);

    let client_that_creates_channel: &mut Client = clients.get_mut(&client_id).unwrap();

    let status: bool = is_channel_create_allowed(client_that_creates_channel, channels, message);
    if status == true {

        //channel_id == key in hashmap
        let unused_channel_id: u64 = (0..u64::MAX)
            .into_iter()
            .find(|key| !channels.contains_key(key))
            .unwrap();

        let msg_parent_channel_id = message["message"]["parent_channel_id"].as_i64().unwrap();
        let msg_channel_name = String::from(message["message"]["channel_name"].as_str().unwrap());
        let msg_channel_description = String::from(message["message"]["channel_description"].as_str().unwrap());
        let msg_channel_password = String::from(message["message"]["channel_password"].as_str().unwrap());

        let is_using_password: bool = msg_channel_password.len() != 0;

        let new_channel: Channel = Channel {
            is_channel: true,
            channel_id: unused_channel_id,
            parent_channel_id: msg_parent_channel_id,
            name: msg_channel_name,
            password: msg_channel_password,
            description: msg_channel_description,
            is_using_password: is_using_password,
            maintainer_id: 0,
            is_channel_maintainer_present: false
        };

        channels.insert(unused_channel_id, new_channel);
        result.0 = true;
        result.1 = unused_channel_id;
    }

    return result;
}

fn is_channel_password_valid(channel: &Channel, password: String) -> bool {
    let mut result: bool = true;

    if channel.is_using_password == true {
        if channel.password != password {
            result = false;
        }
    }

    return result;
}

fn process_channel_join(sender: &std::sync::mpsc::Sender<String>, clients: &mut HashMap<u64, Client>, channels: &mut HashMap<u64, Channel>, websockets: &mut HashMap<u64, Responder>, message: &serde_json::Value, client_id: u64)  {
    let status: bool = is_channel_join_message_valid(clients, client_id, &message);

    let client_id_clone1 = client_id.clone();

    if status == true {

        let msg_channel_id = message["message"]["channel_id"].as_u64().unwrap();
        let msg_channel_id_clone1 = msg_channel_id.clone();
        let msg_channel_password: String =  String::from(message["message"]["channel_password"].as_str().unwrap());

        let status1: bool = does_channel_exist(channels, msg_channel_id);

        if status1 == true {

            let channel_to_join: &Channel = channels.get(&msg_channel_id).unwrap();

            let is_channel_join_allowed: bool = is_channel_password_valid(channel_to_join, msg_channel_password);

            if is_channel_join_allowed == true {
                println!("is_channel_join_allowed == true");

                //set new channel_id for client
                let client_that_wants_to_join: &mut Client = clients.get_mut(&client_id).unwrap();
                client_that_wants_to_join.channel_id = msg_channel_id;

                //check if channel the client is leaving is assigend to him for maintaining
                //in other words check if client that is leavin the channel is the maintainer of it
                //channel structure has maintainer_id in it, client switched channel at this point, maintainer is the same

                println!("is_maintainer_of_channel_leaving_that_channel????");
                let status1: (bool, u64) = is_maintainer_of_channel_leaving_that_channel(channels, client_id);
                //status1.1 id of channel that maintainer is leaving

                let channel_name = channels.get(&status1.1).unwrap().name.clone();
                let maintainer_name = clients.get(&client_id).unwrap().username.clone();

                if status1.0 == true {

                    println!("YES maintainer of channel {} is leaving channel {}", maintainer_name , channel_name);

                    let status2: (bool, u64) = find_new_maintainer_for_channel(clients, channels, client_id, status1.1, true);
                    println!("trying to find new maintainer for channel {}", channel_name);
                    //set new channel_id to client that wants to switch channel
                    if status2.0 == true {

                        let username_new_maintainer = clients.get(&status2.1).unwrap().username.clone();

                        println!("found new maintainer for channel {} username: {}", channel_name, username_new_maintainer);

                        //new maintainer is set within find_new_maintainer_for_channel function
                        //first broadcast channel_join to all clients
                        //then send new maintainer_id info to clients in channel that client left
                        broadcast_channel_join(clients, websockets, client_id, msg_channel_id);

                        let channel_name_relevant = channels.get(&status1.1).unwrap().name.clone();

                        println!("sending id of new maintainer for clients in channel {} username: {}", channel_name_relevant, username_new_maintainer);

                        send_for_clients_in_channel_maintainer_id(clients, websockets, status1.1, status2.1);
                        println!("send_channel_maintainer_id");
                    } else {

                        println!("COULD NOT FIND NEW MAINTAINER");

                        //if there is no maintainer to find (empty channel)
                        //just broadcast channel_join
                        broadcast_channel_join(clients, websockets, client_id, msg_channel_id);
                    }
                } else {
                    //if  the client that left channel was not the maintainer broadcast_channel_join to all clients
                    println!("is leaving channel {} but not maintainer of it {}", maintainer_name , channel_name);

                    println!("broadcast_channel_join");
                    broadcast_channel_join(clients, websockets, client_id, msg_channel_id);
                }

                //following websocket messages are only sent to single client that joined channel, not all clients

                //check if new channel has maintainer
                //if it does not, we will be the maintainers of the channel.
                let clients_in_channel: Vec<u64> = get_client_ids_in_channel(clients, msg_channel_id);
                let client_that_wants_to_join = clients.get(&client_id).unwrap();
                let new_joined_channel = channels.get_mut(&client_that_wants_to_join.channel_id).unwrap();

                if clients_in_channel.len() == 1 {
                    //client is alone in the channel, make him the new maintainer
                    new_joined_channel.maintainer_id = client_that_wants_to_join.client_id;
                    new_joined_channel.is_channel_maintainer_present = true;
                }

                //whether client that joined the channel is the maintainer of new_joined_channel or not,
                //the information about who is the new maintainer only needs to be sent to him
                //its assumed other client have "up-to" date info about who is maintainer
                send_maintainer_id_to_single_client(clients, websockets, msg_channel_id, client_id, new_joined_channel.maintainer_id as u64);


                //inform the webrtc thread about clients channel, so it can update its data
                //so webrtc does not send audio to wrong clients
                //webrtc thread holds copy of structures used main.rs
                send_cross_thread_message_channel_join(sender, client_id_clone1, msg_channel_id_clone1);

                let websocket: &Responder = websockets.get(&client_id).unwrap();

                send_active_microphone_usage_for_current_channel_to_single_client(clients, websocket, new_joined_channel.channel_id);
            } else {
                println!("channel password is not valid");
            }
        }
    } else {
        println!("message invalid");
    }
}

fn is_channel_edit_request_valid(message: &serde_json::Value) -> bool{
    let mut result: bool = true;

    if message["message"]["type"] == false {
        println!("field message.type does not exist");
        result = false;
    }

    if message["message"]["channel_id"] == false {
        println!("field message.channel_id does not exist");
        result = false;
    }

    if message["message"]["channel_password"] == false {
        println!("field message.channel_password does not exist");
        result = false;
    }

    if message["message"]["channel_name"] == false {
        println!("field message.channel_name does not exist");
        result = false;
    }

    if message["message"]["channel_description"] == false {
        println!("field message.channel_description does not exist");
        result = false;
    }

    if message["message"]["type"].is_string() == false {
        result = false;
    }

    if message["message"]["channel_id"].is_i64() == false {
        result = false;
    }

    if message["message"]["channel_password"].is_string() == false {
        result = false;
    }

    if message["message"]["channel_name"].is_string() == false {
        result = false;
    }

    if message["message"]["channel_description"].is_string() == false {
        result = false;
    }


    if result == true {
        //if everything is still allright check if user is trying to edit root channel
        if message["message"]["channel_id"].as_i64().unwrap() == 0 {
            result = false;
        }
    }


    return result;
}

fn is_challenge_response_message_valid(message: &serde_json::Value) -> bool{
    let mut result: bool = true;

    if message["message"]["type"] == false {
        println!("field message.type does not exist");
        result = false;
    }

    if message["message"]["type"].is_string() == false {
        println!("field message.type is not string");
        result = false;
    }

    if message["message"]["value"] == false {
        println!("field message.value does not exist");
        result = false;
    }

    if message["message"]["value"].is_string() == false {
        println!("field message.value is not string");
        result = false;
    }


    return result;
}

fn is_microphone_usage_message_valid(message: &serde_json::Value) -> bool{
    let mut result: bool = true;

    if message["message"]["type"] == false {
        println!("field message.type does not exist");
        result = false;
    }

    if message["message"]["type"].is_string() == false {
        println!("field message.type is not string");

        result = false;
    }

    if message["message"]["value"].is_u64() == false {
        result = false;
    }

    //
    //verify that the value that represents state of microphone is located within the range of allowed values
    //

    let microphone_usage_value = message["message"]["value"].as_i64().unwrap();
    if microphone_usage_value < 1 || microphone_usage_value > 3 {
        println!("microphone_usage_value is wrong {} ", microphone_usage_value);
        result = false;
    }

    return result;
}

fn process_channel_edit(clients: &mut HashMap<u64, Client>, channels: &mut HashMap<u64, Channel>, websockets: &mut HashMap<u64, Responder>, message: &serde_json::Value, _client_id: u64) {
    let status: bool = is_channel_edit_request_valid(message);

    if status == true {
        let msg_channel_id: u64 = message["message"]["channel_id"].as_u64().unwrap();

        let status1: bool = does_channel_exist(channels, msg_channel_id);

        if status1 == true {

            let msg_channel_password: String = String::from(message["message"]["channel_password"].as_str().unwrap());
            let msg_channel_description: String = String::from(message["message"]["channel_description"].as_str().unwrap());
            let msg_channel_name: String = String::from(message["message"]["channel_name"].as_str().unwrap());

            if msg_channel_password.len() <= 30 {
                if msg_channel_description.len() <= 1000 {
                    if msg_channel_name.len() <= 50 {
                        let mut channel_to_edit: &mut Channel = channels.get_mut(&msg_channel_id).unwrap();
                        if msg_channel_password.is_empty() == true {
                            channel_to_edit.is_using_password = false;
                            channel_to_edit.password = String::from("");
                        } else {
                            channel_to_edit.is_using_password = true;
                            channel_to_edit.password = msg_channel_password;
                        }
                        channel_to_edit.description = msg_channel_description;
                        channel_to_edit.name = msg_channel_name;

                        broadcast_channel_edit(clients, websockets, channel_to_edit);
                    }
                }
            }
        }
    }
}

fn is_chat_message_format_valid(message: &serde_json::Value) -> bool {
    let mut result: bool = true;

    if message["message"]["value"] == false {
        println!("field message.type does not exist");
        result = false;
    }

    if message["message"]["receiver_id"] == false {
        println!("field message.channel_id does not exist");
        result = false;
    }

    if message["message"]["local_message_id"] == false {
        println!("field message.local_message_id does not exist");
        result = false;
    }

    if message["message"]["value"].is_string() == false {
        result = false;
    }

    if message["message"]["receiver_id"].is_i64() == false {
        result = false;
    }

    if message["message"]["local_message_id"].is_i64() == false {
        result = false;
    }

    return result;
}

fn send_channel_chat_picture_metadata(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, sender_id: u64, channel_id: u64) {
    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    let sender_client = clients.get(&sender_id).unwrap();

    json_message_object.insert(String::from("type"), serde_json::Value::from("channel_chat_picture_metadata"));
    json_message_object.insert(String::from("picture_id"), serde_json::Value::from(CHAT_MESSAGE_ID.load(Ordering::SeqCst)));
    json_message_object.insert(String::from("sender_id"), serde_json::Value::from(sender_client.client_id));
    json_message_object.insert(String::from("channel_id"), serde_json::Value::from(channel_id));
    json_message_object.insert(String::from("size"), serde_json::Value::from(10000000));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false{
            continue;
        }

        if client.channel_id != channel_id {
            continue;
        }

        if client.client_id == sender_id {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {

                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn send_direct_chat_picture_metadata(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, sender_id: u64, receiver_id: u64) {
    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    let receiver_websocket = websockets.get(&receiver_id).unwrap();
    let sender_client = clients.get(&sender_id).unwrap();

    json_message_object.insert(String::from("type"), serde_json::Value::from("direct_chat_picture_metadata"));
    json_message_object.insert(String::from("picture_id"), serde_json::Value::from(CHAT_MESSAGE_ID.load(Ordering::SeqCst)));
    json_message_object.insert(String::from("sender_username"), serde_json::Value::from(sender_client.username.clone()));
    json_message_object.insert(String::from("sender_id"), serde_json::Value::from(sender_id));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let test = serde_json::Value::Object(json_root_object);
    let data_content: String = serde_json::to_string(&test).unwrap();

    let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
    receiver_websocket.send(Message::Text(data_to_send_base64));
}

fn send_direct_chat_picture(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, message_value: String, sender_id: u64, receiver_id: u64) {
    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    let receiver_websocket = websockets.get(&receiver_id).unwrap();
    let sender_client = clients.get(&sender_id).unwrap();

    json_message_object.insert(String::from("type"), serde_json::Value::from("direct_chat_picture"));
    json_message_object.insert(String::from("picture_id"), serde_json::Value::from(CHAT_MESSAGE_ID.load(Ordering::SeqCst)));
    json_message_object.insert(String::from("sender_username"), serde_json::Value::from(sender_client.username.clone()));
    json_message_object.insert(String::from("sender_id"), serde_json::Value::from(sender_id));
    json_message_object.insert(String::from("value"), serde_json::Value::from(message_value));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let test = serde_json::Value::Object(json_root_object);
    let data_content: String = serde_json::to_string(&test).unwrap();

    let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
    receiver_websocket.send(Message::Text(data_to_send_base64));
}

fn send_direct_chat_message(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, message_value: String, sender_id: u64, receiver_id: u64, server_chat_message_id: usize) {
    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    let receiver_websocket = websockets.get(&receiver_id).unwrap();
    let sender_client = clients.get(&sender_id).unwrap();

    json_message_object.insert(String::from("type"), serde_json::Value::from("direct_chat_message"));
    json_message_object.insert(String::from("value"), serde_json::Value::from(message_value));
    json_message_object.insert(String::from("sender_username"), serde_json::Value::from(sender_client.username.clone()));
    json_message_object.insert(String::from("sender_id"), serde_json::Value::from(sender_id));
    json_message_object.insert(String::from("server_chat_message_id"), serde_json::Value::from(server_chat_message_id));


    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let test = serde_json::Value::Object(json_root_object);
    let data_content: String = serde_json::to_string(&test).unwrap();

    let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
    receiver_websocket.send(Message::Text(data_to_send_base64));
}

fn send_channel_chat_picture(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, message_value: String, sender_id: u64, channel_id: u64) {
    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    let sender_client = clients.get(&sender_id).unwrap();

    json_message_object.insert(String::from("type"), serde_json::Value::from("channel_chat_picture"));
    json_message_object.insert(String::from("picture_id"), serde_json::Value::from(CHAT_MESSAGE_ID.load(Ordering::SeqCst)));
    json_message_object.insert(String::from("username"), serde_json::Value::from(sender_client.username.clone()));
    json_message_object.insert(String::from("channel_id"), serde_json::Value::from(channel_id));
    json_message_object.insert(String::from("value"), serde_json::Value::from(message_value));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false{
            continue;
        }

        if client.channel_id != channel_id {
            continue;
        }

        if client.client_id == sender_id {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {

                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn process_direct_chat_message(clients: &mut HashMap<u64, Client>, websockets: &mut HashMap<u64, Responder>, message: &serde_json::Value, sender_id: u64) {

    let status: bool = is_chat_message_format_valid(message);

    if status == true {

        let msg_receiver_id: u64 = message["message"]["receiver_id"].as_u64().unwrap();
        let msg_value: String = String::from(message["message"]["value"].as_str().unwrap());
        let msg_local_message_id: usize = message["message"]["local_message_id"].as_u64().unwrap() as usize;

        let client_receiver_option: Option<&Client> = clients.get(&msg_receiver_id);

        match client_receiver_option {
            None => {}
            Some(_client_receiver) => {

                //
                //client that will receive message exists
                //

                let client_sender_option: Option<&mut Client> = clients.get_mut(&sender_id); //could unwrap it now

                match client_sender_option {
                    None => {}
                    Some(_client_sender) => {
                        update_chat_message_id();
                        let chat_message_id: usize = CHAT_MESSAGE_ID.load(Ordering::SeqCst);

                        let chatentry: ChatMessageEntry = ChatMessageEntry {
                            message_id: chat_message_id,
                            message_type: 2, //2 private,
                            receiver_id: msg_receiver_id
                        };

                        _client_sender.message_ids.push(chatentry);
                        send_server_chat_message_id_for_local_message_id(websockets, sender_id, chat_message_id, msg_local_message_id);
                        send_direct_chat_message(clients, websockets, msg_value, sender_id, msg_receiver_id, chat_message_id);
                    }
                }
            }
        }
    }
}

fn change_client_microphone_usage(clients: &mut HashMap<u64, Client>, _websockets: &HashMap<u64, Responder>,client_id: u64, new_microphone_usage: u64) {
    let client: &mut Client = clients.get_mut(&client_id).unwrap();
    client.microphone_state = new_microphone_usage;

    if new_microphone_usage == 2 {
        client.is_streaming_song = false;
    }
}

fn process_microphone_usage_message(clients: &mut HashMap<u64, Client>, _channels: &HashMap<u64, Channel>, websockets: &HashMap<u64, Responder>, message: &serde_json::Value, client_id: u64) -> (bool, u64, u64) {
    let mut result: (bool, u64, u64) = (false, 0, 0);

    let status: bool = is_microphone_usage_message_valid(message);
    if status == true {

        let new_microphone_usage: u64 = message["message"]["value"].as_u64().unwrap();

        let client: &mut Client = clients.get_mut(&client_id).unwrap();

        //if datachannel is not active, ignore microphone_usage message
        if client.is_webrtc_datachannel_connected == true {

            //
            //ignore requests about the change of state of microphone requesting setting it to "not active but enabled"
            //if client is streaming music from file
            //this activity stops by stop_song_stream message
            //

            if client.is_streaming_song == true && client.microphone_state == 1 && new_microphone_usage == 2 {
                return result;
            }

            result.1 = client.channel_id;

            let old_microphone_usage = client.microphone_state;
            result.2 = old_microphone_usage.clone();

            if new_microphone_usage != old_microphone_usage {
                change_client_microphone_usage(clients, websockets, client_id, new_microphone_usage);
                result.0 = true;
            }
        }
    }
    return result;
}

fn process_channel_chat_message(clients: &mut HashMap<u64, Client>, channels: &HashMap<u64, Channel>, websockets: &HashMap<u64, Responder>, message: &serde_json::Value, sender_id: u64) {
    let status: bool = is_chat_message_format_valid(message);
    if status == true {

        let msg_channel_id: u64 = message["message"]["receiver_id"].as_u64().unwrap();
        let msg_value: String =  String::from(message["message"]["value"].as_str().unwrap());
        let msg_local_message_id: usize = message["message"]["local_message_id"].as_u64().unwrap() as usize;

        let client_sender_option: Option<&mut Client> = clients.get_mut(&sender_id);

        match client_sender_option {
            None => {}
            Some(client_sender) => {

                let channel_option: Option<&Channel> = channels.get(&client_sender.channel_id);
                match channel_option {
                    None => {}
                    Some(_channel) => {
                        update_chat_message_id();

                        let chat_message_id: usize = CHAT_MESSAGE_ID.load(Ordering::SeqCst);

                        let messageentry: ChatMessageEntry = ChatMessageEntry {
                            message_id: chat_message_id,
                            message_type: 1, //1 channel
                            receiver_id: msg_channel_id
                        };

                        client_sender.message_ids.push(messageentry);

                        send_server_chat_message_id_for_local_message_id(websockets, sender_id, chat_message_id, msg_local_message_id);
                        send_channel_chat_message(clients, websockets, msg_value, sender_id, msg_channel_id, chat_message_id);
                    }
                }
            }
        }
    }
}

fn process_direct_chat_picture(clients: &mut HashMap<u64, Client>, websockets: &mut HashMap<u64, Responder>, message: &serde_json::Value, sender_id: u64) {

    let status: bool = is_chat_message_format_valid(message);

    if status == true {
        let msg_receiver_id = message["message"]["receiver_id"].as_u64().unwrap();
        let msg_value: String =  String::from(message["message"]["value"].as_str().unwrap());
        let msg_local_message_id: usize = message["message"]["local_message_id"].as_u64().unwrap() as usize;

        let client_option: Option<&Client> = clients.get(&msg_receiver_id);

        match client_option {
            None => {}
            Some(_client) => {

                let client_sender_option: Option<&mut Client> = clients.get_mut(&sender_id);

                match client_sender_option {
                    None => {}
                    Some(client_sender) => {
                        update_chat_message_id();

                        let server_chat_message_id: usize = CHAT_MESSAGE_ID.load(Ordering::SeqCst);

                        let messageentry: ChatMessageEntry = ChatMessageEntry {
                            message_id: server_chat_message_id,
                            message_type: 2, //1 channel
                            receiver_id: msg_receiver_id
                        };

                        client_sender.message_ids.push(messageentry);

                        send_direct_chat_picture_metadata(clients, websockets,  sender_id, msg_receiver_id);
                        send_direct_chat_picture(clients, websockets, msg_value, sender_id, msg_receiver_id);
                        send_image_sent_status_back_to_sender(clients, websockets, sender_id,"success".to_string());
                        send_server_chat_message_id_for_local_message_id(websockets, sender_id, server_chat_message_id, msg_local_message_id);
                    }
                }
            }
        }
    }
}


fn process_channel_chat_picture(clients: &mut HashMap<u64, Client>, channels: &mut HashMap<u64, Channel>, websockets: &mut HashMap<u64, Responder>, message: &serde_json::Value, sender_id: u64) {
    let status: bool = is_chat_message_format_valid(message);

    if status == true {
        let msg_channel_id = message["message"]["receiver_id"].as_u64().unwrap();
        let msg_value: String = String::from(message["message"]["value"].as_str().unwrap());
        let msg_local_message_id: usize = message["message"]["local_message_id"].as_u64().unwrap() as usize;

        let channel_option: Option<&Channel> = channels.get(&msg_channel_id);

        match channel_option {
            None => {}
            Some(_value) => {

                let client_sender_option: Option<&mut Client> = clients.get_mut(&sender_id);

                match client_sender_option {
                    None => {}
                    Some(client_sender) => {
                        update_chat_message_id();

                        let server_chat_message_id: usize = CHAT_MESSAGE_ID.load(Ordering::SeqCst);

                        let messageentry: ChatMessageEntry = ChatMessageEntry {
                            message_id: server_chat_message_id,
                            message_type: 1, //1 channel
                            receiver_id: msg_channel_id
                        };

                        client_sender.message_ids.push(messageentry);

                        send_channel_chat_picture_metadata(clients, websockets, sender_id, msg_channel_id);
                        send_channel_chat_picture(clients, websockets, msg_value, sender_id, msg_channel_id);
                        send_image_sent_status_back_to_sender(clients, websockets, sender_id, "success".to_string());
                        send_server_chat_message_id_for_local_message_id(websockets, sender_id, server_chat_message_id, msg_local_message_id);
                    }
                }
            }
        }
    } else {
        println!("chat message not valid");
    }
}

fn send_channel_chat_message(clients: &HashMap<u64, Client>, websockets: &HashMap<u64, Responder>, message_value: String, sender_id: u64, channel_id: u64, server_chat_message_id: usize) {

    let sender_client: &Client = clients.get(&sender_id).unwrap();

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("channel_chat_message"));
    json_message_object.insert(String::from("value"),serde_json::Value::from(message_value));
    json_message_object.insert(String::from("channel_id"),serde_json::Value::from(channel_id));
    json_message_object.insert(String::from("server_chat_message_id"),serde_json::Value::from(server_chat_message_id));
    json_message_object.insert(String::from("sender_username"),serde_json::Value::from(sender_client.username.clone()));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    for (_key, client) in clients {

        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false{
            continue;
        }

        if client.channel_id != channel_id {
            continue;
        }

        if client.client_id == sender_id {
            continue;
        }

        let current_client_websocket: Option<&Responder> = websockets.get(&client.client_id);

        match current_client_websocket {
            None => {}
            Some(websocket) => {

                let json_root_object1: Map<String, Value> = json_root_object.clone();

                let test = serde_json::Value::Object(json_root_object1);
                let data_content: String = serde_json::to_string(&test).unwrap();
                let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
                websocket.send(Message::Text(data_to_send_base64));
            }
        }
    }
}

fn process_sdp_answer_message(sender: &std::sync::mpsc::Sender<String>, client_id: u64, message: &serde_json::Value) {
    let status: bool = is_sdp_answer_message_valid(message);
    if status == true {
        send_cross_thread_message_sdp_answer(sender, client_id, message["message"]["value"].clone());
    }
}

fn process_ice_candidate_message(sender: &std::sync::mpsc::Sender<String>, client_id: u64, message: &serde_json::Value) {
    let status: bool = is_ice_candidate_message_valid(message);
    if status == true {
        send_cross_thread_message_ice_candidate(sender, client_id, message["message"]["value"].clone());
    }
}

fn process_client_connection_check_message(clients: &mut HashMap<u64, Client>, websockets: &mut HashMap<u64, Responder>, client_id: u64) {
    let a: Option<&mut Client> = clients.get_mut(&client_id);
    match a {
        None => {}
        Some(client) => {
            let datetime: chrono::DateTime<chrono::Utc> = chrono::Utc::now();
            let timestamp_now: i64 = datetime.timestamp();
            client.timestamp_last_sent_check_connection_message = timestamp_now;
            //println!("process_client_connection_check_message client updated {}", client.username);


            send_connection_check_response_to_single_cient(clients, websockets, client_id);
        }
    }
}

fn is_client_admin(clients: &mut HashMap<u64, Client>, client_id: u64) -> bool {
    let mut result = false;

    let client_option = clients.get(&client_id);
    match client_option {
        None => {}
        Some(client) => {
            if client.is_admin == true {
                result = true;
            }
        }
    }

    return result;
}

fn process_authenticated_message(client_id: u64,
                                 websockets: &mut HashMap<u64, Responder>,
                                 clients: &mut HashMap<u64, Client>,
                                 channels: &mut HashMap<u64, Channel>,
                                 icons: &mut HashMap<u64, Icon>,
                                 tags: &mut HashMap<u64, Tag>,
                                 client_stored_data: &mut Vec<ClientStoredData>,
                                 message: serde_json::Value,
                                 sender: &std::sync::mpsc::Sender<String>) {

    let message_type: &Value = &message["message"]["type"];


    if message_type == "delete_chat_message_request" {
        process_delete_chat_message_request(clients, websockets, &message, client_id);
    }
    else if message_type == "edit_chat_message_request" {
        process_edit_chat_message_request(clients, websockets, &message, client_id);
    }
    else if message_type == "change_client_username" {
    //let new_desired_username = String::from(&message["message"]["new_username"].clone().to_string());

        let new_desired_username = String::from(message["message"]["new_username"].as_str().unwrap());

        println!("new_desired_username {}", &new_desired_username);

        let status: bool = check_if_username_change_allowed(client_id, clients, &message);

        if status == true {
            let client: &mut Client = clients.get_mut(&client_id).unwrap();
            client.username = new_desired_username;
            broadcast_client_username_change(clients, websockets, client_id);
        }
    }
    else if message_type == "create_channel_request" {

        let is_admin: bool = is_client_admin(clients, client_id);

        if is_admin == true || is_admin == false {
            let status: (bool, u64) = process_channel_create(clients, channels, websockets, &message, client_id);
            if status.0 == true {
                let created_channel: &Channel = channels.get(&status.1).unwrap();

                broadcast_channel_create(clients, created_channel, websockets);
            }
        } else {
            send_access_denied_to_single_client(websockets, client_id);
        }
    }
    else if message_type == "delete_channel_request" {

        let is_admin: bool = is_client_admin(clients, client_id);

        if is_admin == true || is_admin == false {
            let status: i32 = process_channel_delete(clients, channels, websockets, &message);
            if status == 1 {
                println!("clients were moved to root, there is no maintainer for root, trying to find new maintainer for root channel");
                process_channel_delete_continue(clients, channels, websockets);
            }
        } else {
            send_access_denied_to_single_client(websockets, client_id);
        }
    }
    else if message_type == "join_channel_request" {
        process_channel_join(sender, clients, channels, websockets, &message, client_id);
    }
    else if message_type == "edit_channel_request" {
        let is_admin: bool = is_client_admin(clients, client_id);
        if is_admin == true || is_admin == false {
            process_channel_edit(clients, channels, websockets, &message, client_id);
        } else {
            send_access_denied_to_single_client(websockets, client_id);
        }
    }
    else if message_type == "direct_chat_message" {
        process_direct_chat_message(clients, websockets, &message, client_id);
    }
    else if message_type == "direct_chat_picture" {
        process_direct_chat_picture(clients, websockets, &message, client_id);
    }
    else if message_type == "channel_chat_message" {
        process_channel_chat_message(clients, channels, websockets, &message, client_id);
    }
    else if message_type == "channel_chat_picture" {
        process_channel_chat_picture(clients, channels, websockets, &message, client_id);
    }
    else if message_type == "sdp_answer" {
        process_sdp_answer_message(sender,client_id, &message);
    }
    else if message_type == "ice_candidate" {
        process_ice_candidate_message(sender,client_id, &message);
    }
    else if message_type == "microphone_usage" {
        let result: (bool, u64, u64) = process_microphone_usage_message(clients, channels, websockets, &message, client_id);

        let is_microphone_state_change_broadcast_needed: bool = result.0;
        let channel_id = result.1;
        let old_microphone_state = result.2;

        if is_microphone_state_change_broadcast_needed == true {
            let new_microphone_usage = message["message"]["value"].as_u64().unwrap();
            broadcast_microphone_usage(clients, websockets, client_id, channel_id, new_microphone_usage);

            if old_microphone_state == 1 {
                let client: &mut Client = clients.get_mut(&client_id).unwrap();

                //
                //handle situation where client de-activates microphone while streaming song
                //

                if client.is_streaming_song == true {
                    client.is_streaming_song = false;
                    let clients_ids: Vec<u64> = get_client_ids_in_channel(&clients, channel_id);
                    send_stop_song_stream_message_to_selected_clients(clients, websockets, &clients_ids, client_id);
                }
            }
        }
    }
    else if message_type == "client_connection_check" {
        process_client_connection_check_message(clients, websockets, client_id);
    }
    else if message_type == "admin_password" {
        let msg_admin_password: &Value = &message["message"]["value"];

        let current_admin_password: &RwLockReadGuard<String> = &ADMIN_PASSWORD.read().unwrap();

        if msg_admin_password == current_admin_password.as_str() {

            let current_client = clients.get_mut(&client_id).unwrap();

            current_client.is_admin = true;

            if current_client.tag_ids.contains(&0) == false {
                current_client.tag_ids.push(0);
            }

            let public_key: String = current_client.public_key.clone();

            let status: bool = is_public_key_present_in_client_stored_data(client_stored_data, public_key.clone());

            if status == false {
                let mut single_client_stored_data: ClientStoredData = ClientStoredData {
                    public_key,
                    tag_ids: vec![],
                };

                single_client_stored_data.tag_ids = Vec::new();
                single_client_stored_data.tag_ids.push(0);

                client_stored_data.push(single_client_stored_data);

                println!("public key added to client_stored_data");
            } else {
                let status1: bool = is_tag_id_present_in_client_stored_data(client_stored_data, public_key.clone(), 0);
                if status1 == false {

                    //
                    //there is ClientStoredData entry with clients public key, but does not contain tag id
                    //add tag id to it
                    //

                    let option_clientstoreddata = get_client_stored_data_by_public_key(client_stored_data, public_key.clone());

                    match option_clientstoreddata {
                        None => {}
                        Some(value) => {
                            value.tag_ids.push(0);
                        }
                    }
                }
            }

            broadcast_add_tag_to_client(clients, websockets, client_id, 0);
        }
    }
    else if message_type == "start_song_stream" {
        process_start_song_stream_message(clients, websockets, &message, client_id);
    }
    else if message_type == "stop_song_stream" {
        process_stop_song_stream_message(clients, websockets, &message, client_id);
    }
    else if message_type == "server_settings_icon_upload" {

        process_server_settings_icon_upload_message(clients, websockets, icons, &message, client_id);
    }
    else if message_type == "server_settings_add_new_tag" {
        process_server_settings_add_new_tag_message(clients, websockets, tags, &message, client_id);
    }
    else if message_type == "add_tag_to_client" {
        process_add_tag_to_client(client_stored_data, clients, websockets, tags, &message, client_id);
    }
    else if message_type == "remove_tag_from_client" {
        process_remove_tag_from_client(client_stored_data, clients, websockets, tags, &message, client_id);
    }
}

fn process_not_authenticated_message(client_id: u64,
                                     websockets: &mut HashMap<u64, Responder>,
                                     clients: &mut HashMap<u64, Client>,
                                     channels: &mut HashMap<u64, Channel>,
                                     icons: &mut HashMap<u64, Icon>,
                                     tags: &mut HashMap<u64, Tag>,
                                     client_stored_data: &mut Vec<ClientStoredData>,
                                     message: serde_json::Value,
                                     sender: &std::sync::mpsc::Sender<String>) {

    let message_type = &message["message"]["type"];

    if message_type == "public_key_challenge_response" {

        let is_messsage_valid: bool = is_public_key_challenge_response_valid(&message);

        if is_messsage_valid == false {
            websockets.get(&client_id).unwrap().close();
            websockets.remove(&client_id);
            clients.remove(&client_id);
            return;
        }

        let current_client: &mut Client = clients.get_mut(&client_id).unwrap();

        if current_client.is_existing == true {

            //
            //client sends public key to server at the time of authentication
            //server generates random string, encrypts the string with clients public key
            //server then verifies if the client really is the owner of public key
            //"if the public key is really yours, client, please, decrypt and then send back this randomly generated string that I will send you. you will have no problem telling me what I sent you, if its really your key"
            //

            if current_client.is_public_key_challenge_sent == true {

                let status: bool = is_challenge_response_message_valid(&message);

                if status == true {
                    let message_decrypted_public_key_challenge_random_string = message["message"]["value"].as_str().unwrap();

                    if message_decrypted_public_key_challenge_random_string == current_client.public_key_challenge_random_string {

                        let public_key: String = current_client.public_key.clone();

                        current_client.channel_id = 0; //root channel
                        current_client.is_admin = false;
                        let connection_id_string: String = current_client.client_id.to_string();
                        let mut default_name: String = String::from("anon");
                        default_name.push_str(connection_id_string.as_str());
                        current_client.username = default_name;
                        current_client.is_authenticated = true;
                        current_client.message_ids = Vec::new();

                        current_client.tag_ids = Vec::new();

                        let status123: bool = is_public_key_present_in_client_stored_data(client_stored_data, public_key.clone());

                        if status123 == true {
                            current_client.tag_ids = get_tag_ids_for_public_key_from_client_stored_data(client_stored_data, public_key.clone()).clone();
                            let admin_tag_id: u64 = 0;
                            if current_client.tag_ids.contains(&admin_tag_id)  {
                                current_client.is_admin = true;
                            }
                        }

                        let datetime: chrono::DateTime<chrono::Utc> = chrono::Utc::now();
                        let timestamp_now: i64 = datetime.timestamp();

                        current_client.timestamp_last_sent_check_connection_message = timestamp_now;

                        let websocket: &Responder = websockets.get(&client_id).unwrap();

                        let current_client_username: String = current_client.username.clone();

                        send_authentication_status_to_single_client(websocket);
                        send_channel_list_to_single_client(channels, websocket);
                        send_client_list_to_single_client(clients, websocket, current_client_username);
                        send_icon_list_to_single_client(icons, websocket);
                        send_tag_list_to_single_client(tags, websocket);
                        send_active_microphone_usage_for_current_channel_to_single_client(clients, websocket, 0);
                        process_client_connect(clients, channels, websockets, client_id);
                        send_cross_thread_message_create_new_client_at_rtc_thread(sender, client_id);
                    }
                }
            }
        }
    }
    else if message_type == "public_key_info" {

        let is_messsage_valid: bool = is_public_key_info_message_valid(&message);

        if is_messsage_valid == false {
            websockets.get(&client_id).unwrap().close();
            websockets.remove(&client_id);
            clients.remove(&client_id);
            return;
        }

        //
        //message is valid, safe to unwrap values
        //

        let message_verification_string: String = String::from(message["message"]["verification_string"].as_str().unwrap());
        let public_key_modulus_base64: String = String::from(message["message"]["value"].as_str().unwrap());

        let status1: bool = is_there_a_client_with_same_public_key(clients, public_key_modulus_base64.clone());

        if status1 == true {
            println!("cannot connect, there is still client that has same public key...");
            print_out_all_connected_clients(clients);
            websockets.get(&client_id).unwrap().close();
            websockets.remove(&client_id);
            clients.remove(&client_id);
            return;
        }

        let current_client: &mut Client = clients.get_mut(&client_id).unwrap();

        if current_client.is_existing == false {
            println!("public_key_info == current_client.is_existing == false ");
            //sometimes situation needs to also remove the client, return and stopping of code execution is not enough
            return;
        }

        if message_verification_string == "welcome"  {

            //
            //public key is assigned to client struct at the time client connects even if client is not authenticated
            //keep that in mind
            //

            current_client.public_key = public_key_modulus_base64.clone();

            let modulus_decode_result: Result<Vec<u8>, DecodeError> = base64::decode(public_key_modulus_base64.clone());
            let mut rng = rand::thread_rng();

            match modulus_decode_result {
                Ok(result) => {

                    //
                    //create public key from from modulus and exponent (n and e). In this case it is easier approach than creating the public key from PEM or DER
                    //modulus is sent to server from client as part of public_key_info request
                    //exponent is same for every lemonchat client, its known and its 3.. because rusts BigUint couldnt do simple BigUint::from(3), exponent had to be constructed from byte array
                    //

                    let exponent_bytes: [u8; 1] = [3]; //BigUint doesnt suppoort simple from anymore
                    let modulus: BigUint = BigUint::from_bytes_be(&result);
                    let exponent: BigUint = BigUint::from_bytes_be(&exponent_bytes);
                    let rsa_pub_key_result: rsa::errors::Result<RsaPublicKey> = rsa::RsaPublicKey::new(modulus, exponent);

                    match rsa_pub_key_result {
                        Ok(rsa_pub_key) => {

                            let public_key_challenge_random_string: String = rand::thread_rng()
                                .sample_iter(&Alphanumeric)
                                .take(100)
                                .map(char::from)
                                .collect();

                            current_client.public_key_challenge_random_string = public_key_challenge_random_string.clone();
                            current_client.is_public_key_challenge_sent = true;

                            let to_encrypt_bytes: &[u8] = public_key_challenge_random_string.as_bytes();

                            let rsa_encrypt_result: rsa::errors::Result<Vec<u8>> = rsa_pub_key.encrypt(&mut rng, Pkcs1v15Encrypt, to_encrypt_bytes);

                            match rsa_encrypt_result {
                                Ok(bytes_to_work_with) => {
                                    let base64_result: String = base64::encode(bytes_to_work_with);
                                    send_public_key_challenge_to_single_client(current_client, websockets,base64_result);
                                }
                                Err(error) => {
                                    println!("rsa_encrypt_result error {}", error);
                                }
                            }
                        }
                        Err(error) => {
                            println!("[!] error {}", error);
                        }
                    }
                }
                Err(error) => {
                    println!("[!] error {}", error);
                }
            }

        } else {
            websockets.get(&client_id).unwrap().close();
            websockets.remove(&client_id);
            clients.remove(&client_id);
        }

    } else {
        println!("client  does not exist");
    }
}

fn process_received_message(client_id: u64,
                            websockets: &mut HashMap<u64, Responder>,
                            clients: &mut HashMap<u64, Client>,
                            channels: &mut HashMap<u64, Channel>,
                            icons: &mut HashMap<u64, Icon>,
                            tags: &mut HashMap<u64, Tag>,
                            client_stored_data: &mut Vec<ClientStoredData>,
                            message_text: String,
                            sender: &std::sync::mpsc::Sender<String>) {

    let decrypted_message: String = get_data_from_base64_and_decrypt_it(message_text);

    //received decrypted metadata content needs to be trimmed

   let json_string_to_parse: &str = decrypted_message.as_str().trim_matches(char::from(0));
   let json_message: serde_json::Result<serde_json::Value> = serde_json::from_str(json_string_to_parse);

   match json_message {
        Ok(value) => {

            let current_client: Option<&mut Client> = clients.get_mut(&client_id);

            match current_client {
                None => {}
                Some(client) => {
                    if client.is_existing {
                        if client.is_authenticated {
                            process_authenticated_message(client_id,
                                                          websockets,
                                                          clients,
                                                          channels,
                                                          icons,
                                                          tags,
                                                          client_stored_data,
                                                          value,
                                                          sender);
                        } else {
                            process_not_authenticated_message(client_id,
                                                              websockets,
                                                              clients,
                                                              channels,
                                                              icons,
                                                              tags,
                                                              client_stored_data,
                                                              value,
                                                              sender);
                        }
                    }
                }
            }
        }
        Err(error) => {
            println!("error {}", &error);

            websockets.get(&client_id).unwrap().close();
            websockets.remove(&client_id);
            clients.remove(&client_id);

        },
    };
}

fn send_tag_list_to_single_client(tags: &HashMap<u64, Tag>, responder: &Responder) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_tags_array: Vec<serde_json::Map<String, serde_json::Value>> = vec![];

    for (_key, tag) in tags.iter() {

        let tag_name: String = tag.name.clone();

        let mut single_icon_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
        single_icon_object.insert(String::from("tag_id"), serde_json::Value::from(tag.id));
        single_icon_object.insert(String::from("tag_name"), serde_json::Value::from(tag_name));
        single_icon_object.insert(String::from("tag_linked_icon_id"), serde_json::Value::from(tag.icon_id));

        json_tags_array.push(single_icon_object);
    }

    json_message_object.insert(String::from("type"), serde_json::Value::from("tag_list"));
    json_message_object.insert(String::from("tags"), serde_json::Value::from(json_tags_array));
    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let test = serde_json::Value::Object(json_root_object);
    let data_content: String = serde_json::to_string(&test).unwrap();

    let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
    responder.send(Message::Text(data_to_send_base64));
}

fn send_icon_list_to_single_client(icons: &HashMap<u64, Icon>, responder: &Responder) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_icons_array: Vec<serde_json::Map<String, serde_json::Value>> = vec![];

    for (_key, icon) in icons.iter() {

        let base64_icon: String = icon.base64_icon.clone();
        let mut single_icon_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
        single_icon_object.insert(String::from("icon_id"), serde_json::Value::from(icon.id));
        single_icon_object.insert(String::from("base64_icon"), serde_json::Value::from(base64_icon));
        json_icons_array.push(single_icon_object);
    }

    json_message_object.insert(String::from("type"), serde_json::Value::from("icon_list"));
    json_message_object.insert(String::from("icons"), serde_json::Value::from(json_icons_array));
    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let test = serde_json::Value::Object(json_root_object);
    let data_content: String = serde_json::to_string(&test).unwrap();

    let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
    responder.send(Message::Text(data_to_send_base64));
}

fn send_channel_list_to_single_client(channels: &HashMap<u64, Channel>, responder: &Responder) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_channels_array: Vec<serde_json::Map<String, serde_json::Value>> = vec![];

    for (_key, channel) in channels.iter() {

        let mut single_channel_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
        single_channel_object.insert(String::from("channel_id"), serde_json::Value::from(channel.channel_id));
        single_channel_object.insert(String::from("parent_channel_id"), serde_json::Value::from(channel.parent_channel_id));
        single_channel_object.insert(String::from("name"), serde_json::Value::from(channel.name.as_str()));
        single_channel_object.insert(String::from("description"), serde_json::Value::from(channel.description.as_str()));
        single_channel_object.insert(String::from("is_using_password"), serde_json::Value::from(channel.is_using_password));

        json_channels_array.push(single_channel_object);
    }

    json_message_object.insert(String::from("type"), serde_json::Value::from("channel_list"));
    json_message_object.insert(String::from("channels"), serde_json::Value::from(json_channels_array));
    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let test = serde_json::Value::Object(json_root_object);
    let data_content: String = serde_json::to_string(&test).unwrap();

    let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);
    responder.send(Message::Text(data_to_send_base64));
}

fn send_webrtc_sdp_offer_to_single_client(responder: &Responder, sdp_offer_value: String) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    let json_value_object: serde_json::Value = serde_json::from_str(sdp_offer_value.as_str()).unwrap();

    json_message_object.insert(String::from("type"), serde_json::Value::from("sdp_offer"));
    json_message_object.insert(String::from("value"), serde_json::Value::from(json_value_object));
    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let test = serde_json::Value::Object(json_root_object);
    let data_content: String = serde_json::to_string(&test).unwrap();
    let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);

    responder.send(Message::Text(data_to_send_base64));
}

fn send_authentication_status_to_single_client(responder: &Responder) {

    let message_to_send: serde_json::Value = serde_json::json!({
        "message" : {
            "type": "authentication_status",
            "value": "success",
            "is_voice_chat_active" : true,
            "stun_port": 3478
        }
    });

    let data_content: String = serde_json::to_string(&message_to_send).unwrap();

    let data_to_send_base64: String = encrypt_string_then_convert_to_base64( data_content);

    responder.send(Message::Text(data_to_send_base64));
}

fn get_client_count_for_channel(clients: &HashMap<u64, Client>, channel_id: u64) -> u32 {
    let mut result: u32 = 0;

    for (_key, client) in clients {
        if client.is_existing == false {
            continue;
        }

        if client.is_authenticated == false {
            continue;
        }

        if client.channel_id == channel_id {
            result = result + 1;
        }
    }

    return result;
}

fn process_client_disconnect(clients: &mut HashMap<u64, Client>, channels: &mut HashMap<u64, Channel>, websockets: &mut HashMap<u64, Responder>, client_id_of_disconnected: u64, sender: &std::sync::mpsc::Sender<String>) {
    broadcast_client_disconnect(clients, websockets, client_id_of_disconnected);

    let status1: (bool, u64) = is_maintainer_of_channel_leaving_that_channel(channels, client_id_of_disconnected);
    let did_maintainer_just_disconnect: bool = status1.0;
    let channel_id_of_disconnected_client: u64 = status1.1;

    if did_maintainer_just_disconnect == true {
        let status2: (bool, u64) = find_new_maintainer_for_channel(clients, channels, client_id_of_disconnected, channel_id_of_disconnected_client, true);
        let is_new_maintainer_found: bool = status2.0;
        let new_maintainer_id = status2.1;

        websockets.remove(&client_id_of_disconnected);
        clients.remove(&client_id_of_disconnected);

        if is_new_maintainer_found == true {
            send_for_clients_in_channel_maintainer_id(clients, websockets, channel_id_of_disconnected_client, new_maintainer_id);
            println!("send_channel_maintainer_id");
        } else {
            println!("process_client_disconnect COULD NOT FIND NEW MAINTAINER");
        }
    } else {
        websockets.remove(&client_id_of_disconnected);
        clients.remove(&client_id_of_disconnected);
    }

    //message for webrtc thread, so webrtc also cleans its data
    send_cross_thread_message_client_disconnect(sender, client_id_of_disconnected);
}

fn encrypt_string_then_convert_to_base64(input: String) -> String {

    let mut bytes_to_work_with: Vec<u8> = input.into_bytes();

    //VI is same for every key
    let iv: [u8; 16] = [90, 11, 8, 33, 4, 50, 50, 88, 8, 89, 200, 15, 24, 4, 15, 10];

    let a: RwLockReadGuard<Vec<String>> = ENCRYPTION_KEYS_CONNECTION.read().unwrap();

    for single_key_string in a.clone().into_iter() {

        let mut sha256 = Sha256::new();
        sha256.update(single_key_string.into_bytes());
        let key = sha256.finalize();

        let mut cipher = AesCtr::new(&key.into(), &iv.into());
        cipher.apply_keystream(&mut bytes_to_work_with);
    }

    let base64_result: String = base64::encode(bytes_to_work_with);

    return base64_result;
}

fn get_data_from_base64_and_decrypt_it(base64_string: String) -> String {
    //println!("get_data_from_base64_and_decrypt_it");

    let iv: [u8; 16] = [90, 11, 8, 33, 4, 50, 50, 88, 8, 89, 200, 15, 24, 4, 15, 10];

    let mut encrypted_data: Vec<u8> = base64::decode(&base64_string).unwrap();

    let aa: RwLockReadGuard<Vec<String>> = ENCRYPTION_KEYS_CONNECTION.read().unwrap();

    for single_key_string in aa.clone().into_iter().rev() {

        let mut sha256 = Sha256::new();
        sha256.update(single_key_string.into_bytes());
        let key = sha256.finalize();

        let mut cipher = AesCtr::new(&key.into(), &iv.into());
        cipher.apply_keystream(&mut encrypted_data);
    }

    let s = String::from_utf8_lossy(&encrypted_data);

    return String::from(s);
}

fn handle_messages_from_webrtc_thread_and_check_clients(receiver: &std::sync::mpsc::Receiver<String>, websockets: &mut HashMap<u64, Responder>, clients: &mut HashMap<u64, Client>, channels: &mut HashMap<u64, Channel>, sender: &std::sync::mpsc::Sender<String>) {

    for thread_channel_message in receiver.try_iter() {
        //println!("{}" , thread_channel_message);

        let json_message: serde_json::Value = serde_json::from_str(thread_channel_message.as_str()).unwrap();

        if json_message["type"] == "sdp_offer_from_webrtc_thread" {
            let msg_client_id: u64 = json_message["client_id"].as_u64().unwrap();
            let sdp_offer_value: String = json_message["value"].to_string();

            //println!("sdp_offer_value -> {}", sdp_offer_value);

            //reborrow?
            for (_key, client) in &mut *clients {

                if client.is_existing == false {
                    continue;
                }

                if client.is_authenticated == false {
                    continue;
                }

                if client.client_id == msg_client_id {
                    let websocket: &Responder = websockets.get(&msg_client_id).unwrap();
                    send_webrtc_sdp_offer_to_single_client(websocket, sdp_offer_value.clone());
                }
            }
            println!("new client connected");
        }
        else if json_message["type"] == "ice_candidate_from_webrtc_thread" {
            let msg_client_id: u64 = json_message["client_id"].as_u64().unwrap();
            let ice_candidate_value: String = json_message["value"].to_string();

            //println!("ice_candidate_value -> {}", ice_candidate_value);

            //reborrow?
            for (_key, client) in &mut *clients {

                if client.is_existing == false {
                    continue;
                }

                if client.is_authenticated == false {
                    continue;
                }

                if client.client_id == msg_client_id {
                    let websocket: &Responder = websockets.get(&msg_client_id).unwrap();
                    send_ice_candidate_to_single_client(websocket, ice_candidate_value.clone());
                }
            }
            println!("ice_candidate sent");
        }
        else if json_message["type"] == "peer_connection_state_change_from_webrtc_thread" {

            let msg_client_id: u64 = json_message["client_id"].as_u64().unwrap();

            println!("peer_connection_state_change_from_webrtc_thread msg_client_id: {}", msg_client_id);

            let msg_peer_connection_state: u64 = json_message["value"].as_u64().unwrap();

            // set clients microphone_usage
            //(if peer connection state change was caused by disconnect)
            //by the time this message is received, it is possible for client to no longer exist in HashMap
            //check client_option with match

            let client_option: Option<&mut Client> = clients.get_mut(&msg_client_id);
            match client_option {
                None => {
                    println!("clients.get_mut(&msg_client_id) {} -> return == None weird", msg_client_id);
                }
                Some(client) => {

                    let client_channel_id: u64 = client.channel_id.clone();

                    if msg_peer_connection_state == 3 {
                        println!("client.is_webrtc_datachannel_connected = true");
                        client.microphone_state = 3; //microphone disabled, but ready
                        client.is_webrtc_datachannel_connected = true;
                    }
                    else if msg_peer_connection_state > 3 {
                        client.microphone_state = 4; //audio disabled
                        client.is_webrtc_datachannel_connected = false;
                        client.is_streaming_song = false;
                        send_cross_thread_message_client_disconnect(sender, msg_client_id);
                    }
                    broadcast_peer_connection_state(clients, websockets, msg_client_id, msg_peer_connection_state);

                    //
                    //after peer connection state message has been sent to all clients attempt to regain webrtc peer connection with the single client by creating new webrtc client with all event handlers
                    //

                    if msg_peer_connection_state > 3 {

                        //
                        //webrtc connection is re-established by removing old connection entry from audio_channel.rs and creating new
                        //

                        send_cross_thread_message_create_new_client_at_rtc_thread(sender, msg_client_id.clone());

                        //
                        //send up-to date channel info about client, after creating the client, so client does not appear to be in root channel when he nessecarily does not have to be there
                        //

                        send_cross_thread_message_channel_join(sender, msg_client_id.clone(), client_channel_id);
                    }
                }
            }
        }
    }
    check_clients(clients, channels, websockets, sender);
}

fn get_websocket_port_number() -> u16 {
    let mut result: u16 = 0;
    print!("[*] enter websocket port: ");
    std::io::stdout().flush().unwrap();

    let lines = std::io::stdin().lines();
    for line in lines {
        match line {
            Ok(value) => {
                match value.parse::<u16>() {
                    Ok(value) => {
                        result = value;
                        break;
                    }
                    Err(error) => {
                        println!("[!] error {}", error);
                    }
                }
            }
            Err(error) => {
                println!("[!] error {}", error);
            }
        }
    }
    return result;
}

fn handle_setup() {
    println!("[i] This is initial setup. What needs to be specified includes exact number of keys to be used (following with entering of individual string key values) , WebSocket port and admin password. The keys serve purpose of protecting server with password from random connections and also encrypt transmitted data on top of already existing encryption. After sucessfull setup, server can be joined by opening client.html, specifying ip address, WebSocket port and keys that server was setup with");

    print!("[*] enter how many keys to use: ");
    std::io::stdout().flush().unwrap();

    let mut number_of_keys_to_use: i32 = 0;

    let lines = std::io::stdin().lines();
    for line in lines {
        match line {
            Ok(value) => {
                match value.parse::<i32>() {
                    Ok(value) => {
                        if value > 0 {
                            number_of_keys_to_use = value;
                            break;
                        } else {
                            println!("[!] at least 1 key is nessecary");
                        }
                    }
                    Err(error) => {
                        println!("[!] error {}", error);
                    }
                }
            }
            Err(error) => {
                println!("[!] error {}", error);
            }
        }
    }

    println!("[i] {} keys will be used", number_of_keys_to_use);


    for n in 0..number_of_keys_to_use {
        print!("[*] enter key {}: ", n);
	std::io::stdout().flush().unwrap();

        let lines = std::io::stdin().lines();
        for line in lines {
            match line {
                Ok(le_string) => {
                    let a = &mut ENCRYPTION_KEYS_CONNECTION.write().unwrap();
                    a.push(le_string);
                    break;
                }
                Err(error) => {
                    println!("[!] error {}", error);
                }
            }
        }
    }

    print!("[*] enter admin password (used for gaining privileges): ");
    std::io::stdout().flush().unwrap();

    let lines = std::io::stdin().lines();

    for line in lines {
        match line {
            Ok(value) => {
                if value.len() > 0 {
                    let admin_password: &mut RwLockWriteGuard<String> = &mut ADMIN_PASSWORD.write().unwrap();
                    admin_password.push_str(value.as_str());
                    break;
                } else {
                    println!("[!] admin password can not be empty");
                }     
            }
            Err(error) => {
                println!("error {}", error);
            }
        }
    }

    let admin_password: &RwLockReadGuard<String> = &ADMIN_PASSWORD.read().unwrap();
    println!("[i] admin_password is set to: {} ", admin_password);
}

fn check_clients(clients: &mut HashMap<u64, Client>, channels: &mut HashMap<u64, Channel>, websockets: &mut HashMap<u64, Responder>, sender: &std::sync::mpsc::Sender<String>) {

    let mut clients_to_delete: Vec<u64> = Vec::new();

    for (_key, client) in clients.iter() {
         if client.is_authenticated == false {
             continue;
         }
          if client.is_existing == false {
              continue;
          }

        //timestamp_now - > seconds
        let datetime: chrono::DateTime<chrono::Utc> = chrono::Utc::now();
        let timestamp_now: i64 = datetime.timestamp();

        if client.timestamp_last_sent_check_connection_message + 120 < timestamp_now {
            println!("closing connection with client: {}", client.username.clone());
            let websocket: &Responder = websockets.get(&client.client_id).unwrap();
            websocket.close();

            clients_to_delete.push(client.client_id.clone());
        }
    }

    for client_id in clients_to_delete {

        process_client_disconnect(clients, channels, websockets, client_id, sender)

        //broadcast_client_disconnect(clients, websockets, client_id);
        //you need to check maintainer.. If you are disconecting maintainer of channel, pick new one..
        //clients.remove(&client_id);
    }
}

fn main() {
    handle_setup();

    std::thread::spawn(move || {
        stun_server::start();
    });

    println!("[i] stun server running on port 3478 (Can be changed in stun_server.rs) ");

    //used for receiving messages in webrtc thread, sending from main thread
    let thread_messaging_channel: (Sender<String>, Receiver<String>) = std::sync::mpsc::channel();

    //used for receiving messages in main thread, sending will be done own way
    let thread_messaging_channel2: (SyncSender<String>, Receiver<String>) = std::sync::mpsc::sync_channel(10000);

    let sender1: Arc<SyncSender<String>> = Arc::new(thread_messaging_channel2.0);

    std::thread::spawn(move || {
        audio_channel::webrtc_thread(&thread_messaging_channel.1, sender1.clone());
    });

    let mut websockets: HashMap<u64, Responder> = HashMap::new();
    let mut clients: HashMap<u64, Client> = HashMap::new();
    let mut channels: HashMap<u64, Channel> = HashMap::new();

    let mut icons: HashMap<u64, Icon> = HashMap::new();
    let mut tags: HashMap<u64, Tag> = HashMap::new();

    let mut client_stored_data: Vec<ClientStoredData> = Vec::new();

    let root_channel: Channel = Channel {
        is_channel: true,
        channel_id: 0, //0 is root channel id
        parent_channel_id: -1,
        name: "root".to_string(),
        password: "".to_string(),
        description: "this is default entry channel".to_string(),
        is_using_password: false,
        maintainer_id: 0,
        is_channel_maintainer_present: false
    };

    //insert root channel to channels
    channels.insert(0, root_channel);

    let admin_tag: Tag = Tag {
        id: 0,
        icon_id: 0,
        name: "admin".to_string(),
    };

    //insert root channel to channels
    tags.insert(0, admin_tag);
    update_tag_id();

    let admin_icon: Icon = Icon {
        id: 0,
        base64_icon: "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAACXBIWXMAAAsSAAALEgHS3X78AAACU0lEQVQ4jX2TX0hTYRjGf0eOicqUydEES9L+iHQRnNafu0ASYayrXWiS0S4HFQQhCQMzDEUGQUzIGyOJbrr03LSI/hFY5DBLhyut3CkkN9eZ7sx5bHVx9LgD0nv1vc/3PA/Py/d+AmYJLV3+fC67BsBGLst2bRo5CmszpwPw8fkzAUAAhI5AMF/RcNJGnBn02vobj6MAVJWXAnDlWjsRRXEIHYHg393Eo6Nu8iX1OJ0i0Q+fCfQ84erYe/ZJEmoiQVlpOb2BSxRlM9p/xQDNxw7TP9DG3Ysu1ESCaHzJ4hcVinV1GtklAeB0injdIbzukHUvuyTGw+MATMbmARC3hQDGyyCX+85QWduI1x2i4UIvAF53H+GJIfSsztj9IF+4vpNgXV/D3+mjpeUcajxjG8ff6cPf6bNhajxj8a0ED15NWoTK2kYcVfUAdLsP2MSNTUeB1za+aOR0dHUaR1k5sksiLzjQUinCE0M28eL8AgtzM8guieWv73YMAA41nyB6r52bg11oKylWV5aZWzOXKp02X0ksLqa6uoazrQaPHoao840Rm3phGhw/cpCnW/PHPk2hpdOMDM/aEnT3nLaM1HiGvdovs/+d+GGRlha/AjAyPEvrbcVMkDNXeeiWl/6BNpLJpM24COC7GrcALZ0GQCrbA0BFSQli3gAgXsBbSZrLJADFssezAVA69xaA83fCJPQNAPStDwbwpr8DgGzTKQAiiuIQASOiKAJQIXs8GkDs2zxVNfst8fpqCiO7WigUtk2tQ+FYssfzZxeciKLUAT8LsX+oaO/ttIYBtAAAAABJRU5ErkJggg==".to_string(),
    };

    icons.insert(0, admin_icon);
    update_icon_id();


    let websocket_port: u16 = get_websocket_port_number();
    let event_hub = simple_websockets::launch(websocket_port).expect("failed to listen on port 8080");

    println!("[i] websocket server running on port {}", websocket_port);
    println!("[i] initial setup done");
   
    loop {

        //everytime server handles websocket event, handle_messages_from_webrtc_thread_and_check_clients is run
        //TODO: put handle_messages_from_webrtc_thread_and_check_clients in its own thread

        handle_messages_from_webrtc_thread_and_check_clients(&thread_messaging_channel2.1, &mut websockets, &mut clients, &mut channels, &thread_messaging_channel.0);

        match event_hub.poll_event() {
            Event::Connect(client_id, responder) => {
                println!("A client connected with id #{}", client_id);
                websockets.insert(client_id, responder);
                let mut single_client: Client = Client::default();
                single_client.is_authenticated = false;
                single_client.client_id = client_id; //datachannnel is identified by client_id, unsigned short, this will cause trouble, find other way to identify datachannels later
                single_client.is_existing = true;
                single_client.is_public_key_challenge_sent = false;
                let datetime: chrono::DateTime<chrono::Utc> = chrono::Utc::now();
                let timestamp: i64 = datetime.timestamp();
                single_client.timestamp_connected = timestamp;
                single_client.microphone_state = 4;
                clients.insert(client_id, single_client);
            },

            Event::Disconnect(client_id_of_disconnected) => {
                println!("Client #{} disconnected.", client_id_of_disconnected);
                process_client_disconnect(&mut clients,
                                          &mut channels,
                                          &mut websockets,
                                          client_id_of_disconnected,
                                          &thread_messaging_channel.0);
            },

            Event::Message(client_id, message) => {
                //println!("Received a message from client #{}: {:?}", client_id, message);
                match message {
                    Message::Text(websocket_message) => {
                        process_received_message(client_id,
                             &mut websockets,
                             &mut clients,
                             &mut channels,
                             &mut icons,
                             &mut tags,
                             &mut client_stored_data,
                             websocket_message,
                             &thread_messaging_channel.0);
                    }
                    _ => {}
                }
            },
        }
    }
}
