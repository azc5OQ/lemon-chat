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


#[derive(Default,Clone)]
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

#[derive(Default,Clone)]
struct ClientStoredData {
    public_key: String,
    tag_ids: Vec<u64>,
    username: String,
}

#[derive(Default,Clone)]
struct Client {
    client_id: u64,
    is_authenticated: bool,
    is_admin: bool,
    timestamp_connected: i64,
    //timestamp_last_sent_chat_message: i64,
    timestamp_username_changed: i64,
    timestamp_last_sent_check_connection_message: i64,  //todo use differnt type of spam prevention, overall number of websocket messages received over certain period of time instead of monitoring cooldown for every time of message
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

fn get_tag_ids_for_public_key_from_client_stored_data(client_stored_data: &mut Vec<ClientStoredData>, clients_public_key: String) -> Vec<u64> {
    let mut result: Vec<u64> = Vec::new();

    for data in client_stored_data {

        if data.public_key == clients_public_key {
            result = data.tag_ids.clone();
        }
    }
    return result;
}

fn get_public_key_from_client(clients: &mut HashMap<u64, Client>, client_id: u64) -> String {
    let current_client: &mut Client = clients.get_mut(&client_id).unwrap();

    return current_client.public_key.clone();
}

fn find_username_for_newly_joined_client(clients: &mut HashMap<u64, Client>, client_stored_data: &mut Vec<ClientStoredData>, default_name: String, client_id: u64) -> String {


    let public_key: String = get_public_key_from_client(clients, client_id);

    let mut result: String = String::from("");

    //
    //first find out if username is not saved in clientstoreddata under public key
    //

    let is_public_key_present: bool = is_public_key_present_in_client_stored_data(client_stored_data, public_key.clone());

    if is_public_key_present == true {
        let single_client_stored_data = get_client_stored_data_by_public_key(client_stored_data, public_key.clone()).unwrap();
        result = single_client_stored_data.username.clone();
        println!("username found in clientstored data");
    } else {

        for x in 0..5000 {

            let mut is_username_present_in_client_stored_data: bool = false;
            let mut is_username_present_in_clients: bool = false;

            let mut new_username: String = default_name.clone();
            new_username.push_str(x.to_string().as_str());

            println!("checking if username {} can be used for newly connected client", new_username);


            //
            //check for duplicate username in clients that are present and in clients that have their username saved in client_stored_data
            //

            for (_key, client) in clients.clone() {
                if client.username == new_username {
                    is_username_present_in_clients = true;
                    break;
                }
            }

            //
            //check client_stored_data only if value of is_username_present_in_clients is still true at this point
            //it would be pointless to check client_stored_data if is_username_present_in_clients would be false,
            //

            if is_username_present_in_clients == false
            {
                for data in client_stored_data.clone() {
                    if data.username == new_username {
                        is_username_present_in_client_stored_data = true;
                        break;
                    }
                }
            }

            if is_username_present_in_client_stored_data == false && is_username_present_in_clients == false {
                result = new_username.clone();
                println!("find_username_for_newly_hjoined_client found username {}" , result.clone());
                break;
            }
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


fn send_poke_message_to_single_client(websockets: &HashMap<u64, Responder>, client_id: u64, client_id_to_poke: u64, poke_message: String) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("poke"));
    json_message_object.insert(String::from("client_id"),serde_json::Value::from(client_id));
    json_message_object.insert(String::from("poke_message"),serde_json::Value::from(poke_message));

    json_root_object.insert(String::from("message"), serde_json::Value::from(json_message_object));

    let current_client_websocket: Option<&Responder> = websockets.get(&client_id_to_poke);

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


fn send_maintainer_id_to_single_client(websockets: &HashMap<u64, Responder>,  channel_id: u64,  client_id: u64, maintainer_id_to_send: u64) {

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    let mut json_message_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_message_object.insert(String::from("type"), serde_json::Value::from("channel_maintainer_id"));
    json_message_object.insert(String::from("maintainer_id"),serde_json::Value::from(maintainer_id_to_send));
    json_message_object.insert(String::from("channel_id"),serde_json::Value::from(channel_id));

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
    let mut json_message_object: serde_json::ïar|Strmng, seûde_{ûon::Wéluí? =dseúfe_{sî;:Mcp::new()»

 u  jwo_messawu_osjecukÿsuvt(surëng:;froï("tyúg"©l serfe_sÿî:»Şelue::şrom("mkcwphoe_usagı"mm;
 1 ¨êsn_mgsûawe_ïbject¿insurwlÛtrëng~ºfvïm9"ï}iuît_iì"m=serìeßjson~ºvelue~:fúmhclient_idim»    {son_}ísseÿu_obêecü.knÿeşvªsuríngº;fro("cyanngl_ùd")ısurfuêso~;Şalıe:~îúïm¨ûègnnel_d)m;
1   json}íswwge_object®mswút(Surmg:;îvom("şalue"9lsõvde_jwon::Value~»frmìéërophongßusage));M*
¨u  json_rÿot_objegö?ínûívu(ßuvïîw:»fv}9"messëge»+< sesfenwn::Velue;:îúom9json_oessqgeobêecı));  ¸dfor (_kgy¬dcnment} ën1clyenvwd{M
    "1ä éf1g}iínt?kw_gxmstmg¨?=¹falsı {];03u     ıd" continue;¨ dd d""}ŠM*     ¨ ¨iî c|iew.is_eıtìíticaved`½=dfalûe{MN¨¨          coîtiue»N "¨`   ¢}**t¨ " f ¨mf"microrhoıßusewÿ ==uw¨{¯* ¨   ¹d¨ª 1d//ûf¹we wenü0toäsınd mîfosmíüion eâouvîwrgùking ¨øıwh to`tc}{m1onìy¨sent"ıï"cìkents in actuul gjannel
    3 d     éÿ ëlienu~ÿhanel_id a=¨shénnwl_id ûª d1¹f 1 " ""  ¨3gÿnténıe;
  d   fdì3  ]
 ¨ " "`u}Š]*ìd ª"11 ?/yfìcìéíü.cìéent_{ı"=}ªclignüid1{
d  "   ªo/¨¨`dco~tinÿe;  ¨ f"  //ıŠ
   d ¨ fìwu gurúïnts}kent_wefsocïeô: ottoî<&Şustonıír? = wubscketû.get(fcıéenv.slkínt_kd«;

 ¨ f  d¹matgjdcusrunş_cléíîv_÷ıfsïgoet1d  0 d¹"ì ddÎoníd=?d{}N""¨""     d1wo}u8wuwsoc{et9 ?>¨{

 0  d 1 1"wd "d lgt1jÿï_voovßobnect1:¨Ícp<Svsyng,"şelue¿"=dîûoî_vïubjeët?slongì);

  ì"     "" "u 1leu"uest = serue_jÿon;Vclue~ºïbjegu¨jûonßsoov_obêísvw);
 fª ¨ 3q     u dnet"ıcuù_cïnugnt:dÛtrínï = seÿtı_jsoî::toûtsiï(fueÿt)?unwúaq*);MŠıd " u¨ "¹ 3   ¨ngüªdqvé_to_send_wawívt:¹Wusmnï1} gncrypş_strkng_thwsonşurwtï_bíse¿v(¨díôa_conöånt«M
u     1 "î  f 3¢webwc{ít.senulMgsûaïe:ºTeyu(dcvgßıseîu_bısu¾5)«;N 3¨1   1ì1 3}
 3d  ¨1¨}/›    }Š}
¯ÿî msßmcyîşaéîer_of_ÿhqnnul_lïavinw_t}ët_shanngn(chaînels:¹&}uşª]qwhap|u66, Ãhannel¾, cliwnt_iu_uo_cjeck:¨ı6¼«¨o>"(booì,îu¾4o ûM
 d¹¨lut ÿıt1resulv (bol}1w6¼¹3½ (wansel`0); 11¨ÿ/cheînNN   ¨fov¨9ßëe}. cèanngl)¨yn cèsÿnulû {_Í*" ª  d dkw"chaÿnul®iwÿsyénnel ?="falwí {
  d  d1 "¨  conüinwu;]Š    1"  }
MŠd d 11 uëî cûsnelniw_cyùnwl_maknwéyner_pseûunt == wvıe {*  3  " "    yîwcèuînel.}ùkîuaéneúkd ½= c}yet_iì_t_checo{_ı ¨ 1¨  d "d   uûuwwìt®¸ =ftrue;› 3u  1 ¨3d   1 îreÿ}lvn11ı"cjenneî.claÿîe}_id3;      ì1  ì     bveùkN1"1  """"ìd }M›1 "¨d¨ d}/    }]NM
  f»úetuwn"ruwult/*}
Šfn find_wwmakntakníw_fïs_cèÿÿníı(slmwnüs:f&ouüd{csyosr<u64. ë}kenv¾?fshqnnels: fmuÿdHeÿnMùø=ı6üı1Chcnwn>,1ëu_oî_cliwîüßunét_diwcïnîgcwgf:du?4,1chsnıìíu_tofid~ u64."mindßvnï_clienu_vhat_nefv_disëonneëtídº bol)ì¯1(booì="u¾u« {/;ª¨¹dìet"muw`úísult:"(boì,1wvı) =¨lîslsenì0©;
 dì1//üvdto"îynu"neÿªoaintamnerÍN
 1¹ yf ïmnı_vhíÿlienv_v}ét_ìeftßdywëonuctídì½1trıwu{
 f¨ddd"dfr (ßëgy, sliïnt+"mn1cléents 
d¹ f f   ìd1/?kf1clygnı"hesìsame¹cyenne}_{d cw ëloent¹thau"ks leeving gîu ovfcuvúínuclyeîu"d u d îª¨1 mî"slignw?glénnul_iìf=="cyanníl_idßüo_îénw"&f¹éußof_cloewßwaü_uésconnecıedd!=dëlienşnclyentid¹{ b d 1     0u¨11rgwulü?0 ı"ürıg;   d/?foÿîf tje1maiüeknerª¨ı   ¨¹î3ì  ¨1" resulun» = ïîieÿt.slyínu_iw;11¯ogliínş_{í¿cligv_mddof níÿdíeinteiír
¨3 dd    u  "   fríwk;
¨  1 d ¹1dì ; 1¨ " ff}/Š  d3}uïìwe1{M
q u "¨ì fïr¨(_e{?ìcÿiïnÿï i¨siets û?
  ` 1""  ud ¯?iî¨clmenudyaw1sëmí¨ûhqnnu}ßid1eû¹clienv thqu ms luûvog1qnd îtdswsrenu_c}kunu
d"  "1 ¹1  "kfbcìmeunchénılùìf=¹claîîelídüo_îinì"M
1 ¨    ³"d1udê  veww}u?0 = true  "d?/fïwnd thed}ciîtwynws
 d1 d»¨ d 113""dresultn1 ="sléenu.cnigntÿiî;" ¯¯slient_yì/cliínt_yf"f¹neÿ3}ainüañîíwïd0d ì     " î ¹!brgu{»
 ¨1 1¨  ı ı }N1¹ " ª"113  }› d  //ëf¨we fowddthe1íéitéier/
 d ¹yf òewılt.t =dtrwgd{N¨  ÿ1î¨ îÿvu(_ïey,1chenníl)dmndcyanîelû1MŠ " ªª "d    éw}cyannel_id_üï_fmÿd ==dchcnngì®cùanÿ}_{ddŠ "d1fd ¨  d¨d d¹wêgnnelnme{ÿtcyÿevëıª=îr}sult®u;d/osetdïíinuaëír"mì
 1  ¨ w    ¨d3 wcèannín.kû_cnanemaintù{nuw_pvuwınt1=1uvwg?
 uì 1 """u  ÿMd 9î¨  »?ì¨u ı"ense {

ì ªù f 1nuş aa"? cùg~ne}s.gít_uv}&channul_ıîşï_ÿyîd+»*¨    """mïvchugad{MN   1 ¨u¨d"1 Nonud}> {3d " "f    ìî ""øsùntlé(î#!!!ÏcÎîoİ"SÍU ms_slanne}_mí{nwainïs_úúusïnv uo falwe?¨GANNOT FMnD CHANNE]e#!!3);/ d¨1 "d"    }N1  ±"  w  ""somí(vélue}d=~ ó
 dd3 ddì¨ 31d  ¨şıîÿu.ıaitqynus_if ?`1;
   1 ¸ ¨d  ¸  1 vqluínis_ëjannelmqintaiîer_ıríwwnv } fqlse;Š  ¨¹    1 ¨ `ª  púoîtl~!("settiîÿ ys_chcnîel_míinteinerpresínu vo faısídfor`glaÿÿelf{}" , veluenîame.clne9));
 1¨  "      }Š       d}
    }Í

 0d retwrnqsesult;
}_Nfn suucûoss_tùreeu_meswagu_ãîyuntßìmscoînegt(senfís:dfstd~:syîcº~mğsc::Seîdev<Strëng>, clyunt_iìº u7¼©d{/
d ¨ ìet outènûon_rïotßoê{ect~ sgrdu{sonº:Map<Surinï.dsgúìu_jwon::Va}ue> ı¹sgrfe_êûoî~ºÍaø::ne÷(m;¯
N d ¨nwon_roït_bjecü.kîsgrt(Sıúynÿ:;wsïmlªt}üeî), serfg_json~:Vaìue~~fvom(ªëlignt_dkscïnnecôfo);
 d¨ js_rÿow_objíct?insevtnSüréng:~froí}"clxent_ád"}n3ûerdg_jsïn~:Valwe;~fvm*ãlientyd)+;*o*d ¨ låt¹îetq_contgt; Svrkïdıìsírde_jsonş:t_svréngn®jsnrooş_oêêíct).uîwserlí»M
*  ¨ senuer¿síndıfaüa_sonvenu)uxqecv}ª";
*»fn sunì_crossuhråad_mtssãgucruùte_new_clienu_avÿúvcßthríadèsender: 7svu::sync:»opsë::ûedus<String>, clment_md: u64)"›MÎd¨ª lut0}ut nsonßroot_obnecü: serdejsïn~;Mcø<Stronÿ,dsíòfe_jsn::velıe> = sgvdw_nsonº:Mãs::îeÿ9);/n
± 1 nsn_sïotßobjeët.énsíru(Sÿrinï::frÿm»"vùqe"mì`serıg_json:~Vqìug::from9"c}yent_cïînect"9;ÿŠ    nso_òov_ob{ıct.knsert(Stúénï::froo(³c}iet_id")} sírdejson»;vélue;:frÿm9c}iív_if));NN   "|etddété_contenw: Stsingf="seÿue_jsn~ºvostúing(&jûï_úoot_oêjewt).uwvap(m;/N 1  ÿenues.send(data_coîtent).e|ğect¹"î)»
}
Nfn sentßïrss_vhrgùf_ïgûûëge_sdp_awÿerlûuder: &wtd:ºs}nc:şmpsc;ºSendur>Strmng?l1sl{entßyd;¨w64, valugº werdg_jûonşºvaluím û/
  ¨ líu ouv jsoî_voot_sâect~"serueßjsoz»Map<Wtving¬ susìe_json~~Value¾1="serdeßjwon:º]aqººew°)»
" "d{son_rÿot_owject.insuúü;Stûéng::fvïo9ft{pw"i, werde_{sï~:;Valueş:fvom("sdpßaîsÿer"9)
 ì""json_root_bzeãt?ynserv;Svúingÿ:from*"cıienv_{u3m, sgúde_jóïnº:Wíìug;:fsom(ï}ient_id9);
  f {soî_voov_ofjíct?inwuvv*Stryg:;wroon"şslıe3«,"wqnug)»
n  d net¨uatq_contånt: Ûvryng`½¨serue_jwon::toûösmng¨®jsïn_voü_objesu+.unwvaø(m;/
  ì seîuer.wend¨äqté_cïîüent+.u|uecu("TODo: psniã mísségı");ß}

f~dsed_ërss_thrueÿ_oesûage_këgceîdûdave;senìur;¹&swd;:synë::mvsc::Sgndev<sôring¿, c}ieÿü_ku:¨uv5,ìvelug: serde_nwoî::véluum1NM›3¨ `|eudíutdjson_rt_objåcüº sesde_jûï:»Maø<ÿtrinwìfsurfu_êson::Vélue>d= seúdu_jsoº~Åav:~níw()»N*1   jû_roov_beÿt.ënsurtìWüring:~îsom*3uyref)½¨ûuódujsnº:vq~uuº:wvo}9bige_candifqüwwrom_slïenv"))ÿ*   1json_root_ïbjggü.énsåût,Sürynï::fsom*"ëlkeüß{d"9¬ sesdejsïn::Vc}weş;fromgnmentid9};/*    nóon_sïïüÿsjuót.insírt(Stving::îwom*3vé~uı"),¨wálug)NN  " nıtfdeüù_conveuş Stskng ½1suşdu_json:~uoßstrynw9&json_úoouïfjísü).uîwrap();“*w uªsudev.wend(dcüé_convent).í}ùíctl"ûunu_ûşoss_thríaf_mwsseweßisgßcùnäidcteäxgupv"i/ıÍ
Šîn sedcvoswßtèúíad_message_chaîneì_{oyî*sedus:"6svf::s}ns~²mpwãººWenueş=Surmng>, c}ignu_iü: ww4,`chanÿíl_id: ı¶6)d/
(¨¨0 åv3íuşdjson_roıofnícü:1sívìe_jûon;:ïat<Stw}îgldseúdejÿoî»:Va}uı>1}¨serdeßsonº:Map~:newl«;/îª d31{woÿroot_obnwcı.knseru¹Svréîg::fro}9"tyte").ªûevfe{sïn;şValwu»;frïm("channeî_nïán")+;›0 ì {wo_rot_oûjgct.mnsuvt(suréngº:îvoÿ(ªglmíît_id3í="ûwwìí_jóº:vcue::fromlcéeît_iu))
ı1"¨jsonrïotoânwwu.ynsuúô(Strinÿ~from¨"glaîneìyd"9,dûerdí_ûson:öşuluu:~wrm(cìenng}_iìo)¯

dw1 leubdaüa_content: Stsingf¿"serdeßnûon~:tï_striÿg(®json_rooı_osjwëüm®unwrap(mû

 1d1sendgr.sõnì(tıta_soîuenü).eyqestî"î)M_ı
nfn broaîëast_clkenudisconnecü(clãews: 7mut Hgÿì]aø¼u64,"cniínt>¬1webscgts:17LqsyMíø<u6¼¬äSgspodıw¾, ëmgnt_mdof_fisgonnígted: u75o "{
" " ìet cnmentïpÿkon: Ottion<&Clienu~ ½¹clienvs.ggt(&wlkent_ydof_dmwcïîîïóuwd9»
*d"" mstgy cliínvßorüioì{ "d"dª  Noní =? {}MÎ" ì"  "dSmí9ïnyenv_ühat_so~ngsôewo =¾ {

`    "¨    dlut out¨îóon_rïot_wjecü:dsgrdeßâûn;:Mër=Strùnï}1óírdw_jso:ºşanug~¨¿ seúdunson:ºoap~:îew(m;;  ¨ "¹    0¨ìåü mwt1nsn_messóïu_osjgcü:"ûgúdejûïn~:Méø<Wuriï,3seöîg_jÿ:»Valug = serdg_jsoî~;Metşnuw 9;/?
wtd1 u 0  " {wo_meûsegu_ê{ucvnmnserv(Wıwkîï:~froml2u{úíªm=0surtg_îsonº;vclıe~fvoo9&clye~t_uksconníûü"í)»
¨  "f  ± d 0json|eswaïu_obníct®insevv(sur}ng:;îrom("sient_id"),serde_json::Value::from(client_that_connected.client_id));

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
        send_maintainer_id_to_single_client(websockets, root_channel_id, client_id_of_connected, client_id_of_connected);

    } else {
        let root_channel: &Channel = channels.get(&root_channel_id).unwrap();
        send_maintainer_id_to_single_client(websockets, root_channel_id, client_id_of_connected, root_channel.maintainer_id as u64);
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
                    Some(websocket) ¿>"
N"     1   1¨"   1¨  ¨1  let¨jso_sït_objectu: ]ép<Stwingl"Waluu> =ujsn_rvßosnecv?cîone9);Mn
  ª"  `    "  d"ÿª ¹ d dlgtltest"="susde_son::Value:;Ob{eëü({sonrïou_object19;o
  1d¨  ª  f¨ d¨ 1 ì1 1¨dleü dqtaïoîtent~¨string =¨serde_jso::toßstring9&tesu©.unÿsep(©;Š " ù ìfª"1"ì d¨3  u"   1let"dqôa_t_senu_basuv5ş Würinï¨½ íÿëvypt_svring_txen_convgrü_v_fase64( ìetaëontent9;/¨d    ì     1` !d d  d  websockeı.senì9Ííssagg::Üext(data_v_ûend_base65))*  df3ì ª¨d1 ì1dî¨u ¹}o11    1ª  ¹"    /
"df1d"  d   }Š¨  1 1" }/
3 1 }
}
Šfn bsoadcéwt_clienv_}werneoe_cêcge9cnkenus:d&Hashoqt<ı65l Cìoe~v~, webwowoívs» &Haÿhous<ww4, Reÿponder>,dsliuü_md:dw649¨/

  ¨ ìeü cliew_osumïnşä_ption<fGìient>¨="gléínts.get(&climnt_id©;M
/Š    oatch"gìignv_ïttkon {*  1¹¨  d_ÿne }¨{ÿ/
    ¹d¨¨Sme(ûlùent_whï_cnewgd_müs_eíí)"?>d{/î`""1     ¨¨¨let mut {sorïoü_objgût: sgrfe_json::Maq<String, serde_{son::Valıe¾d= suvıe_json~:Map::eÿ*);N   » " `1¹f }ev owt êsoÿmísûege_obnect>`ÿwrdg_jsïn::oau=Süwkîg,ªsusdu_{sïnººŞqìwe¾¨= sesıg_nûon::Mar::eÿ(m;*
 ª  ¨¨ d    jsÿn_musségeobjecu®insert(Ûtvéw:~frïïl"vyqg"+l sevde_êson::Vanuï::îsom("ûìiwnt_runa}e"))»N   d"¹¨  " 1jsïn_mgsûage_obêegt.mÿsert*Wtryng::wroí¨înewßwsívnamuf),dserdeßjÿon~ºVaìue;~fsïm*cìkgnt_who_whugetits_na}e.uûername.clonu(9+*`  "`1 ìª3 djsoî_oessageßïbjesv?ynwerşlStréng~şîvïmlsë}kent_éd39,"sgşde_jûon:şvalue::fro}(cliunu_who_chanweu_mts_aoe.clignt_md©)3M
  "   ¨ 1"f jsonúoïtbjectinsevvısuûynï:fromlªmeÿságe"),ısıvde_json::Vauuº:fsoí(json_íessageßÿfjgcu©©;
/
  ¨" ¨ª 0f¨ for (_ke},qïlmeÿt91mn slignts {¯

 "d    11"1  ª1 {w ã}iínt®osßeüisuing"== îelsu {
 1 "f  f 3"   dw 1¨dcontynug;¯
¨  ¨  11  ""f¨1 
Í  ì"1¨  ª" d 3¨1if sìyeu?iseutèuîukûatedd=½`waìsı {M
¹ "1¨¨ ¨ª      ı 1f`sonténue;
 uª  1   " " "  ı
*"¹ d îª`1d 3  d1lgt currentÿclienw_websosoev: ortéon¼®Ògsrondev>î}¨wgbûockeuû.wet»îc}ient.g}ienu_mu9;
;¨  1 "  d   111 }qüÿh cuúrent_cliínt_ÿuûûogket¨{
"  ¨1  " #¹ u ¨  ¨uìnonï =~ *   ¹ 1 ¨ d*11¨"    3Some(wíbscket) }? {
*     "1"t¨    1"   1 d¨dlet1{sîßroÿw_objecuu:1İqr?String?¨Wcluu>3=¨{son_rotßobject.slone*);
îw ¨u¨     ¹"ìu ¨  ª3"  ¨let1uest =¨wesue_json:ßaìuuº:Ofjggt(jwÿ_rïoüßobnect1)»
3u  1 ¨3d   1 î  ¨1d"f 1ıgt"detaÿcontıît; Stòig = seşue_îson:>to_ûtrinw*7tesv+.ınwveù9);N
dd"è    1udd   d»¨   u "psmtln#*"daté_gontent1}", &dataëvít+»ÿ›Î¹ 1ddq ¨1 f " ¨d3""1"3" let"dãuc_voßsgd_base64:fStûmngì ıîsú{üı_strynwthuî_soîwertüïßsgûe64(qdátécnvent;;  2 1¹¨"ìd ddf  1 d 1  "websoûkgş.send(]ewsage::Tg|t(fétatï_ÿeîd_buseş¾}9;
d¨1" ¨dª 1"3ª¨¹d¨  "};`¨¨   d "  1 ¨ 1}Mß   ¨dª3 1dfì}* 1   dì1}Š"fd }/Š}_/
fÿªsend_clienü_lisußt_sïngıecìmınt(cnigüs:¨&muıdYeóh]au>u¾4ì¨G}ieîv. rewøondgv:u&Reÿøondes, ëurrgnt_cléent_uwurnùmg:fStrínw)1{/
1"f1net íut jûo_roou_bjgwtº werdı_jsn::Mat|string¬1suvfg_{wïî:;value¾1=1serue_jÿïî;:Mér;~ngw;9?MŠ"f1 }eüfmuv {sonßmesséïe_ïâjwct~¸ûuòfe_json;:Ísw<Ûtwéng, serıe_nsonº:Valÿe> = sevdeß{ûn~:Maq::new8}» "duluu mıvìjÿon_clmgtsÿãvra{~1Vec=ÿerde_êûïÿ::Méùşsüriîw. serde_nûon:şşalıe>? =¨vuc!{;ŠMN  d îos|¹_ke{?"client)"íîdëlignts1ÿ/
 ¨3 dd  leu mwt sonïe_cléenu_obûugü:3ówruí_json::Íap|suwëîw,1serue_jsïî::Şaìue?d?"ÿesdï_ÿÿon;º]sp:~new¨;;
  1""  uwiîwlíßclmenu_busş.éîûest(Suúinw~»ÿrom("wserqme"),1ûevue_zsonº;Vewu::fsom9c}kuîtnwseraıw.assvr¨mm9;o ¨3 ¹d"±ûogıe_cìíenwßosîegü.iîsevt(Süring;:fsmê"publks_{ey"), serdensonº:Value:~wroí(w}mwt.puslmûßkey?qsstrl))mM
  "1 ¨  singlg_gÿiïnv_ïê{ect.insírt(Sürinw::wrÿm(îs}annílßéìwï=dserìe_nson:şVùlue:wúom(ë}iuît.ÿhınnul_ùd+)»/" 113   synÿle_clkïnü_object.mnsesü¨Surénw::from(»cliínwßid¢m,1werdgnsonº:Vÿ}ÿí:ºÿvm9ïliet.gliíntmdm);

 ¨1dd¨ "lïş out miëspyone_stateş¨u64ı= clmenv.okëúoøhnÿ_swete?
"d1fd ¨ if¨migûúnone_wtewı ?=¹1f{¯›¨ î"1    ¨u1migroplïîestëví ?`º;
1  ¨ w  }Šd3 u ¨  wéngnï_cniuntobject¿{nsuvtlStûinw:;wvoon3miwşoqhong_stëtuf)=æûerÿg{ÿï:ºvanue::wromìmëÿropyoï_stcte)¹/
   ¨ 13ds}nole_c}meÿşïêjısô.kîsgút(Strkngş:fro(fteg_ids"©}¨wgsdejsoo::va}we::from(clíïnv.üqÿmdsîcìone(í+©»/›"1¨1 ` 1{won_c}ieîwsarrãy?úıs{ªsknwleclmeü_objegt)o
    }M
N" `1jso_mísscgg_object?iîûgrtStrkng:ºfrïm("u}pg"«½ sevwe_nÿïn;;valıïÿºîsoı("slyunu_list"m9;
  ¨1jûoî_oeûsawe_b{ígt?iîsert(Svr}ng::fúoo(ÿcìmwnvsf),1serueßjsïÿ::Vauí:~îromªnûon_clients_éûúqy));/
ª   jÿoo_message_obîgsünknseÿu¨Wtring~:ÿûomn"}ocal_usernéme"),suvdu_jsoo:;Şa}we::from9ëurrent_glient_uwírîamunas_svr(}9);
   1jsn_root_sneût.knsırtlStrig::wrïï93meûûege"©. werfe_nûo~value:?nrï}(êson_meÿsage_obêect));* 1 ©ìet3üíwt"?dsírìe_jsonş~Şalue»:Oêjíst(nwonrov_objïcv);1"" ìgt féüq_content:1Striîÿ  serìï_êwïî:ºtosürmï(&tewt©nwnwvap¨);
N ì "ìev datùÿıoßswußbeûevü:ìString"? engr}rt_ûtrongtheîßconvgsttoßbase¾tn data_ëontut)»N  ¨ rustondev.ûeîd(Messcïu::Tuxv(déta_vßsenf_baûev4));/
}M
o_fn¨gheckyf_wserncmeßchénÿï_a}lowed*slyenv_kwşdış4¬  clientÿ: 7muv ÈcwnÍcr=u64, ëlmíîü>, glégtwüïveudaüc: f}uv¹Víc>ßniíntSvoredUauq>= }gwûawe: 7ûgrÿe_jsnş;Vqlwu+1->(bol1{ª_"  1ıet clienv_thqt_wëîvsßto_ïhaîge_usernqog; &Cnienı"=1ënkïnus.wet(&clment_md)nunwrap,);*M
¹dìfìït uatutime:dcèrono::DetgÜime<clronoº:Uuc>¨="chvoÿo~:İüc~;nw(+;/N ìd let3vmmwswampßnoÿ i¾t¹= fcvutùmu.vy}esuéop¨);
¯
    éf1clûentßvhavßwûnvó_vo_ïlqngıusernaoe?üioesvamp_ususname_ëèanged1;ì3¨> timesuuíq_now {İ
 "" 3   retÿún¨false› 1 d}
 1u lív ngw_deûyríd_usernùmu:1Süwkng"= muwûege["}eswaïeª]["new_wserqme".cw_ÿur(+.wnwúcp*9.üo_striîï9)»
 ¨  f new_uesivef_uwesîaíe.yw_gmpty() == trıeì{*  d¨  d¹rïtÿrn falwg;
  1¨ı/_
 "  kÿfnïwßìïsired_usuvÿaoe®}un() > ut {
d   ıd "ruşurn wanse;M
d1d1}+
 ¨1 wïv (_key,1cìient)"inªclíínts ûM
¨"1    ¨ifìslket.usírname ==ªnew_fesiseì_usuvÿame N   ¨  11ì   privln!9"ıserame şaoen,¨unuúù1mn¹cìienus"/?*  u ¹ d "u  retuvn1îése»n   "f¨"1}; 3 `}N
   1wïs uate iî clkenv_stïrídßdatq 
"ì""  d if datq.usírqme =?3ngÿ_feÿired_wseûnamg {d   "¨  d1ìªpúiüìg93userní}eduagn= etryfk"cìmensvoríd_uate3m;/
  1u  31"d"fríwuún1îansu;* ¨1  f" *dd¨1}
   (vetuûn urue»;}/M
fn ks_chcîîwl_cúíatían}oÿud(cnyunt_tygtßcveaugs_cìanní}: &mut Ëìyeîu,(ïèanngns~ufmwt LasjMap>w6¾, Ëjaînıl>,1ıessage;d&suvdí_{son~:Vqlÿu+d-¾ boon¨
d¨ »lev ut úuswıt:3bo}1=¨vrug;

"   o/chgck ûooluowÿ"1 ¨oeu ìatetkme: chron:şDavíTi}e=wèvoÿº:Utc?d½dglrono~:wuc:~no(©»*d1d leÿ¨uiÿesüaït_nowşfi74"= ìatetimí.timestamø(9;
¨d( ofdcìkenü_thcucreévuw_chcnoe}nÿioïsvqmpß}ést_chgîelssected¹«¹1?"uomestémv_oow¨{"u1$ d  resunv1½wfqlse;
î df}
M
"   ?/fkrst cheëk kf¨jûon ÿmelìs1gøist?** ¨ "iw¨íwssqïe["}essagï"]["rqreîu_changl_id"]d=} fclse /N  1ì¨¨3¹srytln!(îfie}ddmgssaïwnpcseîw_ûjanîel_if uïesdnov gùmwtf©»M
f " 11ì reswlt ½1fqìse;
   d}›
"1dwyf"meswégg_fígsûqïe"]û&channwl_cewÿ }? gelwed {*ì¨u   f pvinü}ÿ©*"fiı}d1mıÿsqge.chénnelnamu ìous nÿvlexkstî);
"  d"1ììruûınt =1fqlwe;MŠ ¨¨¨}
    if meÿsaïe[f}íssawe#]["chcnnel_duwcrytüéïn"]w=ı1fqlwe {
 1 ""1  ùrinuln!("fiıìd messéog.gncnîgìßeescr{øtëon does1nÿv g|ísü"9;Nì  "¹"f1rïsult }uîelwe;
 " ì}o›/
 1d¨iwdígssagí_"meûsegef][fslënnïìÿpessworwª] == falsg¹{/
   1¹ " øúinvnn!}3fmeld mgûûegg.cycînuì_paswÿvf doos nov ïxmûü");/
 ¹  ì 1drusw}t = anse;M
 1d }Š.¨ ¨diÿ íeswéïe_"muÿscÿwwİß"péwwîtc{unnel_odf]?ésßm649) }= ÿqnûe1{*"¨d    dswynwln1¨"fieldfıewwcge®peveît_ëhqnnul_éd"wúÿg"tyüïw);
f3 1   lvesw}t ½ffgîsí;M1   }o
Šd1  of messagu["mgûsagef]Ûfëhanneì_name»].és_string¹md==3îélûe"{
  d¨    psmtln!*"fimlì oıssuwg®shaînel_e}eîwşonguüypef);*   ª¨  dveswlv1? false;
¨   ÿM?  1 ig mgÿÿugíÛ"messcgeª]["ëyannel_ugscrystio"İ®is_ûüşyng()3=½wîalÿwd{M
 d   " "prmntlîe("fiílw1oessawu.chcnîel_dgwcrmrüiÿn"wrong type");
d1d ¹¹  resw}t ? fa}sg;/
 1 3
Nf ¨¨éî)}wsséïe["mgssewgf]["chénn}lüawsÿoru"nkwÿstrinï()1==dfínsí"{*d u ¨1ìfqrintìn!lwwielf1meswagí.cèanen_pasûworfdwwong tùşm"m;/Š  uuu   úewunv =¨nù}su;ª"1! }*¯_1 ¨ iw1rgsunüª=?dvvıo {Š 1" " " ìgt msï_pavgnw_cèanÿel_id¨½ íessawe[wmessagª]["qcvgîtëjénngnßídf].éû_y74(}.uwrmş*9;1 ¹d d "}et }wg_clcnnml_neouì=1Süsinw;:wwo}(}essage[foussaÿgf]["sèsngl_namg"]nùwûşrl).unwsqsª©© 1 !"1ì leş3osgßclwnngn_dessripuyon =$Wüûínï:ºfrïm(ogssqge{#mewsswe#][*s}annín_ìescrortknwnasßsvr()?wnwrap());
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

fn is_poke_client_request_valid(message: &serde_json::Value) -> bool {
    let mut result: bool = true;

    if message["message"]["client_id"] == false {
        println!("field message.client_id does not exist");
        result = false;
        return result;
    }


    if message["message"]["client_id"].is_u64() == false {
        result = false;
        return result;
    }

    if message["message"]["poke_message"] == false {
        result = false;
        return result;
    }

    if message["message"]["poke_message"].is_string() == false {
        println!("field message.poke_message does not exist");
        result = false;
        return result;
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

                        send_maintainer_id_to_single_client( websockets, 0, client_id1, maintainer_of_root);

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

fn process_poke_client_request(clients: &mut HashMap<u64, Client>, websockets: &mut HashMap<u64, Responder>, message: &serde_json::Value, client_id: u64) {

    let result: bool = is_poke_client_request_valid(message);

    if result == false {
        println!("is_poke_client_request_valid == false");
        return;
    }

    let message_client_id: u64 = message["message"]["client_id"].as_u64().unwrap();
    let poke_message: String = message["message"]["poke_message"].as_str().unwrap().to_string();

    //
    //cannot poke ourselves
    //

    if message_client_id == client_id {
        println!("cant send poke to ourselves");
        return;
    }


    let client_option: Option<&mut Client> = clients.get_mut(&message_client_id);

    match client_option {
        None => {}
        Some(_client) => {
            println!("send_poke_message_to_single_client");
            send_poke_message_to_single_client(websockets, client_id.clone(), message_client_id, poke_message);
        }
    }
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



fn is_public_key_challenge_response_valëd*messege: &sırdeêson:;Şalıu) m> êool1{Š
d"  letdmuı¨resu}t: fol =1truu;"uu"ùf1messqgï["musseïe"][ªvaìue"] ?= fcÿûe¹óM
d " ü   print}!("}s_publàc_ogy_cyaììege_reûpnse_valiõ oessqgí.tëg_oì dïes ît1í|msü"m;
"ì ¨  d vísult } fése;
 "d1   dúeuurn sesulu»
d¹1d}Š
¨ " ıfìoïsscïe_ªeswcgg"]["vcluı"] }=1fc}wå {N1ì dì  føúénüıîg("is_vublic_{ey_chglneîggresponse_şeliw¨meswagu.c}éent_iì does1noude|ist");
`   d¹ "veÿwìü = fclsí;
     d"1ríuuún rewu}t;M
"  d?Îª¨d1 şutÿú ûewu}u;
}ª
ÿn0is_swbnmg_ogy_mnfo_oíûsawe_valkd;musscgeº &ûerde_json»~Value) /> foold{_ ª"1leu owtdsusult: bol ½ uvwe»o
*f   mfdmwsûawı["meÿscww"_"valuıf == false {Š dd d""dqúmntln!(ªiû_øublic_eyynÿoßíussagg_veìid meûscge®üag_id does nït uxiûtf);   ¢  "1reûuntf=¨fense;/
"  1¹¸  vuÿurnuwísÿnt;
 ¹d¨ÿM
 » ¹if mesûqgg_3ıgsseïe"_îşerkfmcaukïnsıring"İ¨½} false"{N1f ¨ ¨  prkÿtnn!*"ks_public_kuy_mnfo_message_walid meóûaïe.clyeÿt_iu does noü1eøksw"m;
» " d1¹fruswlv"= îslûg
¨d     dretwvî3result»* " u}Š]*ìd ªkw1mussaïeÛªíuûsaïï"]["wùnueª].is_óvrig¹)d=="falûe ûd      »psitlî!n"is_p}sìic_ke}_énfï}ewsaïï_vclyf oessagentégßid"msfoït"ís_wtrùîg3);:q  q    úewultd?¹îalse;*¨d  " ª returî vesuıt;N"d  }/N"u  ¨éî"måûwqïe_"muwspge"][fÿgşifmëetéonwtséng"İ.ks_strmw(}1==0walsud{
" ¨     psintl!9"w_rublkg_kíûinfoíeswage_véìkd 1ouswegg®glmeÿv_mî¨oÿ nüukw_stúiw"+;/Î    ¨   şgsult ?"fc}st;"1      retur şesu}t;
 d "
M¨ ¨dríuurn reûı}v*}
oªfî {w_add_wemoveÿnyıtßtug_mewûegíşalid(ıeûwage: &serìejsïn~;Víluu)"-¾ sooldûİN " uìív¹mwt ríswìş:1fïol = trut_ÍNd ¹dywdmíswage["meûsage"]["taw_ku"]ª==3waìseì{MŠb ¨ ì  1println«lfiw_add_seoïvecëent_wawßmgssawe_vaîiì féelu1messgïı/vqgßyd üouû0nïv ex{ûu");Î1 311  3vesı}ü1?¹false;N1 ¨1ı  drıtwsî¨ríûult;
ı 1f}
N¨   ow muûsawe["ogsscgeª]["cnùentÿët3] ?=`fqlse {
  d ª   pr{ntln1(3mscdfrííoşïßglkent_taï_íewsége_vùìiu`fûílu messgwe.c}ieuiì¹wïes1nov e|isu39;¨ı   d¨ result ½ feìsı»N   ¨  ""reüurn"rwûu}t»¨uì"ª
d d"yf meûsqgí[fïusûcge"]{"vag_oì"]?is_u64l9 ?½ fclsí {/"u¨   `1rrinuln©l"fiu}duïíssûwu.tag_}d yû1nt u66";»*  1 3 ¨1result3}ıfansg;;  " "   ruşwrîuúesulu»/Îu ¨ }¯;/
 ¨  iw1mussaïu[3}essage"]ß"ëlyínt_iff].iwÿuv7¨)¨½= felwe û
3u  1 ¨3trinulî!(îÿmgnd1ıgssege.ënkenıßiu is nt w64"í;
î    ` frısult1?1îansg;ÍN dd¹1 "¨vítuvnèresu}uM
  f»ı
_ " 1vutuvn"resuìt*}Nw"isservgú_sgüwmnïÿßaìı_ewtéw_vanmìlgsscwg: &sgrîujwoî;~wa}ue) -> fooÿd{MÎo
¹¨1¨níı1muv1vwwuluº1soï} = wúıı;oŠ
  u éf¨oewwqge_3messawe3İÛ"ì}noef_ison_yd""== fùnwï {
  1 f   trinvln!*ªfiwlì íeûsagu.cìëel_mì1foíwªnv3ïøùwü");
3`¨¨   drgsu}t¨=1feıse;Nª3 1dfì úgturn wísulüoN  "¨}_/
`»ª"iw mewsegí_"mesûawe"ÿ_"ıinkífßiconiæ3İ.iû_iv½l9d½=1æunsí ÿn1  ¨wu" proîtln#l"wielı¨oessqge®linked_icïn_mu }s nÿtfiv4")ÿo""1  1"fógsulü = fùlse?/
1 3  "w úevurn¹resunv;
dd" 
oŠ
1d"fiwdïïsscge["íusssgew]_"şëï_eíg3] ?=3walûgf{ ¨f 1" 0prmştln!¨ªfiíìd3oewûéwí.tqg_na}w¨weû ïv exist»);M
 d¨    ªvesulv  fù}ûu;o
d 1    `suüusn"vusı}t;Í*ì è }
N"3 "ïîfmeswqge[3íeswagíªİß"taïßîsíe"İ?ks_strmgª) =ıªfaısg1{Š u  1"11øvmntìn«(3üùg_ncg msnt svşénïf);MN 1ì"0 ¨3rewunt } fclseÿŠw 1  ¨  setuûdşesıt;" ff}/Š
d31uîíu uag_a}e:¨ßtrénï }qmçsÿaÿeû"ÿïssqïu3}[vtagßamef]?cs_óuv(©?uîÿrep()?t_wusënï¨¹;
1¨  yf¹ıag_naoenlu(m > 3» ~}duqg_néeng()"= 11{*u¨ d"  "qrùtln#(fvég_qoe ìwnÿtj¹éw1nÿtdaììowwì {ı f¬¨tég_na}e®len8»+_dê  d d"susult =`wanse;M d¨     return¨rwsuu;
d1 dÿ
M
113"returndvesulv;Š}
fn"iwÿiëon_ıø}ad_mesûage_şaliu*owsûegí;u&sgşdıßêwï~;Vqìuem -> îoÿ} {/_Š   ¨}ew¨muı ÿesulu» boïn1?duswe;
¹ d éf"ïeûsage["}ewsaguªİ[wbéóu64iconßvalıe÷İ =½dfqnwe {/N" d¨  ÿ1şúyîı|!9îfie}d mesûaggnfesu64ßicï_vélweªîogw not¨uıisô");
 ddì¨  pÿesult ="fcnûí;
1"ÿ1 3ddvwüurn1vesıltN d¹uÿM
/
 d d{ÿ ouûsggïÛªmïsóage"İ_3fawev4_ëïon_vëıe"]®is_strénw() =½dwasí {MŠ" "ª "f urmvln!("ûssuwt_msoÿ_vqluudksnÿ suşiç"+;]
 ªd1d ¹îúusÿnw1ı¨waìse;
  1   ì úıtwr"sïsulv;
n2  1}

13d"}eü besuv4ÿşrëîg»1Ótréng¨= messcgï{"muwsegef]["sqûu¾tycon_weıue"]?sw_strn).unÿşar*©?ıo_swşiîg();Í*ª"¹"1éw fasuv4stryngîg()"¾17¾½13û* 1  "  dpséntln!l"bawe64_icon_vcluu is1toï1bkg");
  d1  ±"rewulv"?dfélsí;
ud f ª¹ retwrndşíswlt;Šîıª¨}/›
3d11ruuurn result;Š}›ŠM*fş psogussßveoşg_teg_frm_cliínv(ÿléetstorud_dqté: ®ıut Vwc¼ClëentûvïretData>, cìûíts: &mÿt HaÿhMat<u64. Clkínu¾n"webûëoets~ffmıı HgsyMap<uv4, Víspondes~,1tags: 7íut"HashMcp=ı64, Tag~, messawí:¨&suvde_{son;Value, sliunt_id:0u7v)¸{*
¹  d}et yw_valéî;1boÿìd? iûadd_remoşw_gnkent_vqgíusóage_vaìidn&}esûege©;

" 1 éî isßşelkdd=½ îalsg
èd¨ {M
¨    ¨3 return; 1 "}¯

  11ngt¨kw_cìíyn:dboon =1is_cìùentadíãn¨gìéeîts. glmuît_mdm;No
 d  éf ys_adíin¨=? falûï1{İ
311¨ dª tşiîtln!l"cnient ms nït gfmon")»
 "`   " rmturn³Mn    }MŠ
 10 lıtwclieît_yd_to_rgmïví_tag_frï}:"u74"=díeswcwí["mgssaïef]["cniunt}d"İnas_u65().wnwvct(©;
 ¹ªdlut tag{d; u64"?èmíÿsége["mewsïge3]["tég_oì">as_u6¾lm®ıîwrapl);N/Nì¨d |gt ënienu_oùtéonÿ"Oøtin>&muu"W}iunt>u½ slieüs.ÿet_muvì7c|igv_idßto_rumoşe_vag_ÿrom);

 d 1mavëè"cıienü_oøtion {
1" 1    Nonı"=?¨{¯
1 !3   "Some(clmenv) }> {
  "d ¹dìd¨ª lut1uag_optiïn:dOption>®mut Teg> ?¨vews.ïet_mwv¹&tcï_id);* "d ì`    3"mawcn taï_oúwioîdû
 "f1 ¹ 1 f1   1¨noîe =>ª{}
 ¨ 1  »   ¨"  "¸Sÿmg(_vagí`=> ÿ/
"f f 1  "      1  11// ª      13ì ¨    d  ?éwdglieı unfes specmîéeì clkentùd1e|ysus au tég underdûqgëiviud tùg1iu íksts
 1d `   "1d d ¨ ¨1  ooadw taw1id vo"veÿ1if"iwsdîot"qlúeady tèíse
  1 ¨  w    0dt   d /?N  ì  1d0"  d        iî gligntnüag_ùdÿ.ûontainw*&ôag_yìí"}= fclsg¹î"¨¨ª        1d¹  d¨11   println©(fno üeg"tªteleue"9;N d1d11 2   ¨1 3¨f       sgüuvn;*  ª  ì¨"    d ¨"1   }
Î1 " w    ¨ d    11ªdclmgnt.uaï_ids.ûetain(|&x|¨x 1=ìtag_id}
 ¨1 "3    ¨ d   ¨1 1¯¯ ¸ ¨   1" "d3"  u ¹ o/kw trying uï¨semÿşg adooî"ueg? whe one wity1Áw:10,dseü iwadoin üo fùlse
f   ì""  d       0 0/¯;N   "3 "¨ " ı "  d"  ùf tcgyud== 2¨{0àª ª 1ì¨0d1 d1   ì1 d` wliuntis_gd}kn }dfcnsgŠ  1  d1d""   1u  30"}/nŠ3d¨ 1ª "31" " ¨1  f lgt"wtéuus; bool } isßruslicßëwyqvesenvin_cìownt_ûüoríf_dqté}clig~ustosgdßdeta="cliínt.üublig_keù®slïe(í©;N/*1dud 1  d  "   "" ª iî"süaıus =ı walse 
N ¨ 1" `d  1d»1"d ¨    "¨vrinülÿ!("pwblië1kwù { ou1púgsent, thct slowlf noı be1poûÿyb}e®!Mw ítw rgally notdurísenü, uluwídmû3îothig¨toddofdl s}ienv.ğuêìic_{eyncÿïe¹))»îM
 "1ìf 1 "1 ¨ dd   ¨ } else {
1    1¨d  f d ¨"  ¨1d "1prinüle("pwbliwdÿe{ª{0príwínt " n sligu.ruflùëß{ıy?cloe¨)o;/
]Š"1`"u1` d  "    f1¨w"1 "leu ÿtevus1~ bool1= ks_tag_iì_preûeît_iÿ_glíevßstosgddétc(sìíwnt_ûtorud_fatë. clkuntîqubliwoey.cnonu})."tag_mfm;ì¨¨3¹3 11     ì"  1 d " ifªwtavuw¹"=½"trıe {*¨  d" " "¸df d¨¹d f " 11ì   qrintı!9îis_tag_id_pûesgt_ynclieîtwvïveì_ìavaª== false39;
]ª q3  d u d   "ì¨u   f  d "¨u?¯/
d ¹1 1 ¹î 1     ¨ "  3  d1 ¨/?vheûgdis"ClíenuSvorefUíüa1íîvry wiuhdclmínüû¨puslic key, buş dïesdïv cntqin tcg idM
1d  0d ¨¨d  u ì1 1 d      1 //qdd¹tag1id to iıN      #" df  ¨ ¨¨0 2 "3¨ ªd //
 ¹" fdè"¨ 1  dì  "¹"f1 ª d  luuìottioî_cniínvûvorudìaue¨? getßwlieît_stored_dëtcÿêû_tws}mcëey(clienvûtored_uùtc,¨ëlignt.uublig_ke{®ëlone(9+»Í
  "u¹1d" d "      ª d¨("  " mùtcì ttyon_clieÿtsvoreddqta {
¨ "¨  d » ¨` d¨¨ d  1ì1"¹3u¨¨   _îef??u{}
 f d 3¨ dd  1  d  ı1"¨ 1  " ¨d  Somw}waug9¨=> {
f¹dwd"" ¨ dd (  ¨ 1  1 d¨d" ¨1¹ "  ìşuluu.vsg_idsnvetcyn(üfx~"x¨!}wuag_yf);1  f    "  d1 "df""   d ¹d¨  `  ¨ " prùntìn!("reooÿed0tsï¨ÿ". vag_mì);
 1d1    " "   ¨  ı  u1"ª1  ¨  d1}M î ì  du¨   f   "   ª¨  dd  M*1"    " "  ¨   "d131  0 }
 fıìu ¨¨    """ ¨   ı
     u"   03d "1 ì¨ brïéìscst_seışe_üsgfrom_clkent(glmeîts, wífsgkgts=1cnignü_md_vo_reoïvÿvag_fvom, tag_idm;M
¹¹      1  "   u}/
"  1 3"  `df}Š(ª013 1¨ı"d""}M;o

în"ñroïewsßadu_veg_şo_cliínt9clmeîv_ÿvorgd_uaüqşf7muvdŞec|WienvStosgdDétc¾,1clientsº &mwt ]asnMeøşu6t,"Ëliuu>, ÿefsockeüó»1&}utªjqshMùr¾u7<¬ Rwsronugú¾,"tcgó² &mıt1jashoaø>u6t,¨Tag~. esûagı: &seúìeßjson;~Vulue, céet_kf:¨u6¼+¨Mn/Šìdd líü }s_vuliw: nïo} ı1ysßedd_rumovu_clkent_tew_mgwÿawußwanyw(w}essugu);/
N"1  iÿ"ms_vqì{u ?}dfalsgN¹u"ªÿ/N    f 13úíüus;
 "1ì}
;"" ¨lew msqdmi: bn = mwßûìiínü_aìminnclyenvs, gl{unt_iì9*MŠ" ¨ if os_cum}nd==¨fclse1{
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
                Some(_tag) => {

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

                        let username: String =  client.username.clone();

                        let mut single_client_stored_data: ClientStoredData = ClientStoredData {
                            public_key: "".to_string(),
                            tag_ids: vec![],
                            username
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
                send_maintainer_id_to_single_client(websockets, msg_channel_id, client_id, new_joined_channel.maintainer_id as u64);


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
  ª "d  M*    }
}*fn síndßuirectßchau_qésture_metedaõé(clitnôs~"7HasyMaq}u65, c}}gÿt?, wgssïckeus:dêHashïcp<u64, restoÿîeû>l wendür_id: w6u= reweiver_id~dw74)1{Š 1  lgt¨mut json_rov_ob{eët:ªsevìe_êsnş;Mqø|Wtúinw, sírìe_nsoî:~Value>¨? serdenson:~Íaq::new();¨  dıut mıt êsonßmíwûcwgßofêwct"sgrue_nso;:Mau<Stryg, sesìe_îsonººŞaìıí~ = sevde_jso::Map~:neÿ(oN
    lít rwëeivur_websïgket ı websgkttsnget(&receivevßifm.ıÿúap();*ì   let senueú_cìient =1clmentsngwü(®ûedís_ïì}.ınwsqq();¯
 ¹"0jsomgwwqgg_ofject.éîsest9stvknw:;from(ªtyøe"),1surdí_json::Vclıe::fvom}"îksecu_cjet_qigture_ottaìaue2)¡"f  jwon_eûsqÿe_obÿecu?iwert(Sıvkng::fro}("qyëtuve_kff9¬dserde_êsïnº:Valuu:fsm0CÌÍU_MESsAGEßID.loéd*Ovìíring::SeqCst)m);
¨ d jûon_mïsscgg_ïbjegt®mnsert*Svriÿº:fvÿ("suìeúuóernùmíª)=dseûdı_json:ºVanuu:froí(swnìírclmenu.ÿwgrÿame.cìïşe()))11d1nsïnßmesscÿåobject.insert(Suring::from("senwev_id"),¹sívde_{wÿn::Walue::frï}(ûgnwev_id»);N›f 1 json_rïtßofêeët.insevt(Svvíg::wroí("mgswqgíw+ìdsïruu_{snş:Şéìuí::îúom(jsÿn_mïssage_objucı)m;*
 ª  ìítdtest =»surue_êson::Vaìuí::Objecü(nû_vooşÿobjesv);M
  d leü äatc_coşenü; Wtrùîg3= swwde_jsonº:to_strùîg(&tusv©nunwrëp();

f  dıet dcta_uo_sçd_êéÿg6ô»fSüving1} tncryptßsürmnï_tèengoşervßvo_basev5 uqta_wontunt9;/
¨   receiver_wubwocket.sedìïwswaggº:vext(daüéto_sud_bcûe6t)©;NÿnÍ
f¨wgd_dérwstcjít_piëturí*clienvs:"wHpsh]cq<u64, Cliunt>l wuwsocketó: &HesèMét<ı74,"Resøçfgr>= mgûségw_value Wtrmnÿ.1ûunìewid:dıv4ì"òeceivırßmd: u64) {
3 1¨leu íut1jsoî_soot_ïÿnect¨ûgûdw_jsïn:ºïaq~Ûtving, sewdußnwoÿ~;welıu~  sgrdï_json~:Map;;nuw(«;;3  1lítdíwt¨jÿon_mgssageÿofjuct: surfo_jwo2:MatSuúmng, serfeÿjûonº:Vq}ue>"¨óerueß{soÿ:;Íppº:new;©;
ì1 3}ut seceéwísÿebsogkít1=¨wıbsocûevs®ïeüª&receiÿesmu).uîwrar9);]ª  1 lev sendeú_clkgÿtd=îë|yenwsngut*&sender_iî).unwsep();M
"" "jsïn_íïûwage_ofneït®insívt(Wıúig:»îrm("tyvu").1sewug_êûî::Wanue~~fr}9"uyúıct_gèat_pmctuúe"m©;Š d  jûonoesûqgeïrûewt®iûuşv*Sşring~:wrom(»pycüuvï_yì"), swrfe_jwïn:;Value:~wroí CjATßMEsScGÍ_ID.}oad(_rdíving;;SuûËst»;9;
d udjsÿ_essageïsjecu.{nûurt(Str{nÿ::from(3sendgr_usuşaíuª), suûfí_jûonº;valıe::wso}(seîues_client.uÿeúnqíg.glone()}»M;¨ ¨¨json_mesûùgw_ob{eëwninsurş(Süsmng:;ÿroolfweîdgv_ıì"9, sírwe_jsonş;Vaîue::from¹sendur_id++;/Îd ddûsonßmíswegí_objuwtninsgûü(Stwing;~wrom*"valuí"m,"sevdu{son::Vanwí?fşm*íÿûsíÿe_veuí9)*Šd3""{sn_roov_ïsjegv®{nsest(String:~ÿvomìfmıûségíÿ9, suvvg_jsï;:Şqlue;ºîÿo¨json_mísûcgwobnest)9;:¹¨"ìletdvesu } serfg_jsoÿ:~şalue::_bnectljson_rooü_ocjícü)»
 u  ìïu1dateßsonüeît;"sşúùnï ="sesdíßjson::to_süryngı&teût«?uwvíp¨+;

dì1 lívfdatcßuo_senÿÿbase64~ Wtşmng =¹ecrÿptßsvrénÿ_then_gîveút_tÿ_seûe7<}"détíßgntíu+;
f¨ddvgge}verßÿgbsoskeş.sgnd(]esûage;:Uuxt¹dgtg_toßwunubqse76o9;N}Í

fÿ senf_uisecvcèav_oeûsagu*cnients:"&HgûyMqp>w65lªëlygnt?,¨wessocetsş*®Yawèoqt<w67= Vísvnueú~,1oessagí_valıí: Ûür{ngl¹ûuîfes_if:1w¾7 úewíkver_id» uv4,dûerveú_chqtmgswùwí_if: usize) {]Š11 "lut0}utìjÿoîroot_osjgï~~ sgwue_jsïn:~Maø¾Ûırinï½îsírdí_json::Vqlÿe> ıªseûde_jsïn:Mqr;;îew()ª 3m¹let"wtdjwon_oíûwégeofjuïv; ûwrde_jso::oap<wtúnw, ûerue_jûnş:wáuu¾ ? wgrfí_jwºÍup;:new(};/ŠÍ
 ¨ ¨leudrgïe{vïrÿÿebsïs{etf= wíssocoeus.gíul&úucíévev_ium?uwsëp¨©»  11ìet1wıÿder_cnienu1=`cliuÿtw?gut(&sídesyd).unwscq*}»N/
 "1 ûsonoewsége_fneëw.ùnsıúv9wırmnïş:fsïm;ît}øí"©.dwerdí_jsoÿ:~Wulÿe:~fvo}93direct_cnatmuswége"));N1" ¨jwn}essewe_ÿêjecw?{nsert(Stvmng::wrïm(3vanug"m¿ ûerdíß{son::Vqìue::îrom9ogssùgeßwulug©);Šwî1d{woî_messcgï_ÿbjecu?énseúu(Süriÿgÿ:fso}¹"seîfur_uswrna}}"¹,dsårfï_êson::velwe:>wúïm}sínuerclmeÿtnuûewîamenc}one(++m;N¨  ÿ{ÿïoßıewsqïe_osjegt.énsgvtlSuriîg:ºwvoí("sïîdgv_id"©=usurfe_json~şŞaluı::from(senfíú_ùd9+
3dddnûon_}gwsége_ïfjeûu®onsert(Wwûinw»:frïı¨"ÿgsver_ë}qt_mewseïí_ku"«¨sgvìe_jsonº:walueº~wrm¨sevşgrëhcv_}gwsgge_id¹;;_M
Md¹  {s_rooÿ_owîest.knwerşlStrùnï::ÿr}ìª}eûsage"),1serìeÿÿson;:Wëlue::frÿo*jsn_mísssgg_objegt9m;İªª¨ ¹1ìgv¨tgût = sgrdïjsn:~Velue:;Oêígv9json_vootos{ecv);O
   ìîev"ìqıc_cîtínt: ßvúënÿ"=¨sevdu_nson:;toßsvsing*7uïûu;®unwsap*);M
Šd   let dgta_to_wedbase66; Wüsing"= encr}qtßûvrig_vjgn_ëonşert_tobkûe6tl3detíßcvenüïÿ¯Š1"¹ rwguyvus_wgbsos{et.síd©Oíwsaïe:;Teyt9ìats_üo_send_base64)©;/
ıŠM;fn wenu_chqnîelßûhat_qiëtuúe(cîoínts: &HashMéû<u64, clëent>ì wefsockgts:"îjqónoap<ı7¼ldRewvonıır>n"}essage_valıe: Stryng= sendesßid: u64,"cyénnel_id~ u64) {]Î ¨  }et muv jwn_root_objuct: sesuujûon::Mùp<Wurinwl serìo_{soÿº~valıg>d= serfíjwon::Mar;:níw();
   llev }uvªnsoî_messcgu_ïêjesüºdsgrdeßjûon::Maø|Ûtrmnï, ûeúwe_nwon::Vqnue> ¿ serdu_jsoî:~Mcøş;ngw(m;/
    ìıtfsgndíò_ëléínü ="clievs.get(®wgnder_éd)?unwríp*©?/

 ¹ª1jÿo_}íswëggßoîject.insertlStrinï~:fvoo("tùøe"+, serde_json:~value:~îrom93chùnel_cèat_pmcturg"©)»
 d  jûnmussagí_ofnwët.insgrülWtring:;gro}("pmcturg_id"+,"wgvdí_jsoÿº~Vqlue::fso}(CjAvÍEßßAÏE_ID.loaî(Osderiîg~~ÛgsCsv)))»¨¨ì json_ouwseïí_osjecü.inwurv¹Süriÿg:ºfro*"usurqmu").uûesde_{ûon»:Valugş;froo9sundmr_slyenş.userùme.clone()m);
"ª¨"jûon_íesÿage_objesv.ynsert(Sıriï::îrm(3channel_id"ml werde_json::Vaıuí~ºîro}(s}annel_iì))
  ` jûon_mewscggßof{ecü.insgrı(Wvúing::wsoo("vaìue"),3serwejsoî::şwluí~»wroon}eûsqgg_valuí+)»

ª   jsïn_roÿt_oêjecv¯iÿsgru(Stşmg:ÿfvom*fmgssagg"), sesde_{son>:şélue::wsïm¨json_meswaweobjuÿt)+;;
   dîïr¨(_kg}n ólyenu)1in c}ieîvs {
N¹1 ¨ d 1if ûlyet®s_ezistynïd== fqlse ûŠ1  dd  1   usontonwe;İ  " w d¨*
¨      ¨íw ëliet®isauthenticatef1=} wálÿe{N!"  d       coîtmnug;¨  d¹ î ÿ
  d"    iw¨ïn}ent.chcÿneîéìª!= channulßiddû      dd  ¨ gontínug;ª1   1 "1}NN1d11 "  iî1cégnt.clievßidd=="seîdeşßkd {N ¨"1    ¨  ìsontnue;
d    11ª}
M*   1 ¨  lut¹current_cléenu_ÿebsockev> Oqtén>7Respïnder>¨= wïêsocûeüs.guv(&gkenu.ûlmenu_id);.¨¨3  ¹¨fmatcn¨cuvrwnw_client_wessïckut 
ì  d"  "   ¨Nïnı =>1{
 ì""  d     So}e(wíbsgket+3=>¨{/
İ
"  d"  ¹   " 111let jûon_rïïtÿosîícv1:d]ap<ßurmng} Valuw> =fjson_voovofneët.slonul+;
u  3 "d"f ¨3d¨ 1îev3ugsv ½1sevfejsonº;Va}ue::Ojnect¹jsn_rïïw_osnect1+;
 "¨f3d  ¨¨   " d}eüudata_sontev:¨Strig = ÿerdí_json~:tïßstúinoì®tewv+?uwrqp(m;* ( #" ª  ¨" ¨"¹ leuıdqta_to_sed_êasg6v~ Suvÿgd=¨encr{øvstúiÿg_vhwn_cïvwût_wo_bqse¾>*dfata_content+;/
  ¹   1  ¹ì1"1 ¨wessïcoe|.send(Meswugí::víxt9dqué_tÿûgnu_seûevt))MN 31  d" ( ¨¨}/N1d d »¨1}Ÿ
 ¨ î}Îo
wn"qrïcews_dérect_clatßmussagw¨gìmgntsº"&mıudHcshMap¼w7t, Cnienu~ÿ wïbsockíuû: &muv¨{esk]qp>uv4½ªÛuûrndes>¬ oussuïg;d&wuvde_json::vqìwg= senweş_mf: u~4+ {
"   let ûtavuû:¨boïÿd= ís_cèav_gssaïegúíst_wélid9messaïg);*
 î1 if wvgtus ?=duwug"{
N"d  1ì¨¨ıw }ûg_recíkves_md: u6¾w} ouwûsgı{"mísûagg"]ßfrecekvgû_odfİ¿esw6499îunwscpl)»
1d 1   $let1mûg_vqlwu:"Strmîg"}fÛvréï::fúom(messaÿu{wmuûsuwe2][wvelue"İ®us_svr(m.wïwûér(m)»
1 ¹î"1  let¨msg_oce}_íessigıíd:"usí{g1="mewsqïí[3ííssagu"[flogélßííssqge_id"].as_ÿ64ì)?uÿrap9)1qs"uskze;Nd¨ 1d(¨¨letwcìyet_receivuroqtiÿn:"_ptmon<&Ûìment?"½+clmgotû.ïíu(7íswßvïgeiver_yd¹;nMÎ"¨ 1 ªdìmavûjfslëent_ruwímver_ïptkoî *  9d¹ 1d¨" "Noïu =~ª{}
d  d1dª "î¨ªSou9_cëen|_rece{vır+ => {
/
¨¨  ""  u1 d d  //¯Šdd" #1"¨ 1ì   "u¿?gnienş thav ÿilì¨recgivı!míssegu gyists
*"d d  1d  "1 ¨ /¯ŠM
» ¨d d¨¨ d  1ì1"ıwu¨ïliíwßsgnwur_optoon:3Ïpümon=&mut ß}kínu> ?"ïlignüw?wutmwu¨.sendevßmwm;"/¯coulì uîwsap1it¨now
"  ìªu  1(f3 u  mmtch"sliíntûeîdew_optyon û  f    "  du "dfª"  Nonıd½> }Š "   ¹  ¨      " ¹ddSoí¨_ëloentseîder) =~1{
 " " 9 ¨ !ı  u1"ª1  ¨  duqdqtï_ïhat_íessggeßkd()»
 dd  "d"1"    " "  ¨   ªluw1chqt_meswıïu_éì: uskzg ½ CHÉU_MGSSAWgID?oqf9Oşìeriîïş;seqCst¹
3d"d  d " " "  dd$ ìd " lít3snavenusy> chítMeswegeEnürû"? ChatOewsqgeEntvyd{›     "1  "   u " "  1 7"  ddoeûûéïı_{d;¨ëhqv_ogswsgg_ie,Š "9d"ì d3ì  1 "d*dª     ¨  1mewwígeßvypg~ w,¨?ïv1provétel]; e "1  3g  ¨ "¨ 1 `"   "¨   recuivgrßéî: msgßrewu}verßyd*"  ¨(¹1 1  ª"11  ¹"ªu1 ¨};/* 1"¨ª "d"d(ª   ¨ 1"d" " ßclienü_senfes.míssùge_idû®pÿsh*cyetuntry);?Šu1d1"f"¨ "¨"¨ddf"¨ìdd  ¨ûeu_suúvwr_ïîcu_íussùge_if_for_locclmeûwqge}ì(wuêsoc{wtw}dsedur_kd=dcya|_ıgwsqguß{u,"mwg_localß}gûÿcge_ido;;ª¨¨d11 1 1"1ì   ª3"" ¨ dwenfuirest_cyqv_mewwéÿí(ïléenüs,"webûogkutsn sg_waìuu,"wíndír_idn"msw_wgoeyşer_id=3chat_message_id);
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
    else if message_type == "poke_client" {
        process_poke_client_request(clients, websockets, &message, client_id);
    }
    else if message_type == "change_client_username" {
    //let new_desired_username = String::from(&message["message"]["new_username"].clone().to_string());

        let new_desired_username = String::from(message["message"]["new_username"].as_str().unwrap());

        println!("new_desired_username {}", &new_desired_username);

        let status: bool = check_if_username_change_allowed(client_id, clients, client_stored_data, &message);

        if status == true {
            let client: &mut Client = clients.get_mut(&client_id).unwrap();
            client.username = new_desired_username.clone();

            let public_key: String = client.public_key.clone();

            let status: bool = is_public_key_present_in_client_stored_data(client_stored_data, public_key.clone());

            if status == true {
                let storeddata = get_client_stored_data_by_public_key(client_stored_data, public_key.clone()).unwrap();
                storeddata.username = new_desired_username.clone();
                println!("also chaged name in client_stored_data");
            }

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
            let username: String =  current_client.username.clone();

            let status: bool = is_public_key_present_in_client_stored_data(client_stored_data, public_key.clone());

            if status == false {
                let mut single_client_stored_data: ClientStoredData = ClientStoredData {
                    public_key,
                    tag_ids: vec![],
                    username
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

        let default_name: String = String::from("anon");
        let found_username: String = find_username_for_newly_joined_client(clients, client_stored_data, default_name, client_id.clone());
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
                        current_client.username = found_username;
                        current_client.is_authenticated = true;
                        current_client.message_ids = Vec::new();

                        current_client.tag_ids = Vec::new();

                        let status123: bool = is_public_key_present_in_client_stored_data(client_stored_data, public_key.clone());

                        if status123 == true {
                            current_client.tag_ids = get_tag_ids_for_public_key_from_client_stored_data(client_stored_data, public_key.clone()).clone();
                            let admin_tag_id: u64 = 0;
                            if curúïnu_cnmgnt.teÿ_iäw®gonvué~s¨7aìmhnßv`giu¸1 +1 " ì""¹ª "  u ü ì"1d d1  1u0ì1 çwwwgÿt_glégwt®ys_qdmmî =õüûwe»M
1d¨"0 uªÿª1¹¨d du"1ı1   3 " }  õ1¨"u" ı"ddw1  3b¨¨ 3 }ªŠ 1 1è    d¹ fd3 3ì¨ `ª  nıtîìaıeüyouºdgjú~n~:fítíTime~ë{vno~:İüs~t=dëhvoo:~wüs;ºnïw9)»¯
¹¨1 d¹1d"ì¹` ª"ªdÿ ìfî"1nívdü{tÿvéop_noÿ3 yÿ4d=ÿuqtgwíe.ıo}ístı|p~í»
Œ›¨f ` 0 w01d 0 1     fª"uûuvwenu}clxenünui÷ísta}p_}esuÿÿg~t_ïheco_sonÿervìon_messegg u»tk}eÿwéíp_nw;/Î/  ` ¨ df3 ¨u ¨    d ¹  flgu ÿïóÿoïëdu:ìwVïûuoÿäww1½ websÿckıösngew*&w}ou~w_id).uwúép(9;*gN"`ÿ 1 d" d¨  ¹  ì u q1 ¹låt2cırrînş_cníınt_uûïsna}e;"Wtsynï0? cuúsínü_slûeîv.usgrnaegnçoîg1¹;
ß`"u313u     ıd"      ì1 ûuìßuutlgnş{ëívyn_süaşwÿtÿ_sqgîw_wıåíîu1websogoíum;N¨ "ìdª¨1  ¨ `1( 1 b¨ì111sífßrùí~äîìksvßş~sïnïntbliwwlc}ùÿneìs¿ we÷wïcëgté3
¹tìª0¹d `»"¹¨01du ¨±dfì1ÿgnìßëliwnşİìkwv_üîßûïîgnı_cl{eîş¹ÿlxents.1ÿuswoëkív¬ cwûúïnücigtuwåsfumemwN    d  ö¨3ddd `¨ ¨¹ èdd sw÷ı_icflist_tî_sùægt_glhÿnş mwÿşs= ÿewsïcëwt¹;Mî ¨d3 "  dd¹ îdî3d "ı qªtsenî_uaî_níwvÿv_s{fwlí_ëìüuîv(üëgsl wıbsïëket+*¨ ¹qù¨f0 "   ªd1¨¹dt  1 seÿìastkşíoacrúùïneßuse÷í_fïscıröïît_cñqnnçl_tn_s}nïìí_cnmenuîcníuôs,ıÿíssoc{ut.õ0¡;ˆ d  qd"¹ì  f 1 "¨f d""ª progeûsgdmışt]ïonnõwvcîıenüûîªcîùîÿïlwl wgbsoczutÿ½"ï}mgîô_éd©;o
¹¨d""ª"" ¨   u1w w310uw síd_wrowû_vhríau_lwóséwgwgrgetenıÿÿëmenuétvüc_tèúısu(ógugv,"ë~ãenıidï»oî duìu"u ì ¨d310b""ìt}/ª   ìª   0 "" 3u}?
du31 2dd  d }]
1 ı ª1ud"d¨ÿ/
d1ì`ı}sí1if2mewûéwgtóså ¿ »øwwlig}kuù_onfoÿêŠ u" " d¨líş"ís×}esÿsıfeİvùlhdº"îowì1½`ow_üıb}kã_ëeymfïßmeswuïí÷ÿaid(®ogÿû`wg¨;\*1"1õu uìmw"ùw_ísûûegg_vc|{î1}= welse û?*1  î1`s3 ¨1"ÿufûsêeüû.wevl®ïlkïfô_}u9.uÿwrcşy9nëïse(mw	¨d" " 1d32 »wşbsïcouus.rgoïÿun7wì{enıyì9;¯* ìd»¨1   3ì3syutsnrgíÿşı;¿glientßõıi»ß¹ d » "1èª ¨úevıvnİ1fd1" dª}
o 0wúu01d?/>ª2 î` ¨  ?.oıswáïïıùw welmw,ªscşe toduîwrat1wíluüs“ä2ª"d"2"¯¿MŠ¯Îw`» d dìlïuîmgûûegdWÿürñfiûít{on_stv}ng:±Stwy~g¨½sÛtv{ng::wvo9}uwóqïı["míûsqïefÓfÿíroî}ûítmn_ûvrkng"İ?úsûwû(u.ÿnwúuü*+)»\
d df1w dlÿt3sıblës_ïgy_åowunus_féóe74: Utrmg"½¨Ûwr}îg::wúÿíª}d÷srşe[3|tûwave³}[wşé}ıÿw].ás_÷uv(9¿õnwraø::);Íß/
 1 31¨3flít sweıus3z"âçl û ãó_ôhuşw_ø_ëlieÿußwíwhısaïwğuêìis_{eñ8cıïınws,±pıw}iwßoïy_íodwæww}ÿñwÿww÷ûnïîu8om;Í
›¹±3õ 0±°éwfstauuÿ1 ıùdvvuuıó:ddd ª2ft`ı¸ súi÷ü|e(fcpnî÷t ïïnggü ıhgse1{óìsö{nnıglmeÿubvùeü luóìsá~u1uuf}iãöûíñ??÷"+;YN¹1bd2ft±è1¨àvúot_ouw_bl|coængûõuæßólãıÿısîû|yevwó©;î°3f¨d3wî1"3fwgbû~çëuvsngíwn7c}iånt_mdo.ıÿwsåş«¿ï}şsíı9;/*1u3f`"1î31¢¨ÿerÿ÷ïëıuw®òtmowt¨®ënëwnwWf»;»  s¨±¹¸"ıw2dgkdôÿsçmove(6ëÿjfîu×iì8Îf 3 u"d¨"d  rítuónÿÎ ê1`àw ìÿ¯ß•
  d¨1f`¹}ïüqcwúúınü×c~iåtşì&mut clyunÿ`="ÿıienü÷êweõ_ıvì&s}itnuıyì9®wnwvqr¨}³]ªÎ»ª"11b  }wìrıvrånuÿc}áüÿtâùwåøoÿtingf=ÿuîanûe0İN1dì°3ºwş ¨3îüöyîtìu*¢uufìmw_ktñ_infÿ¨?? susvÿnöëniu÷tªis_upıûumÿgw= æqìwõ 3{•ª`q"f¹f d üs`«/ûoåtkmusssévwaüiçn"~ıeuówvï3q}wo¢rçíowí1t{t3v}yeîş.±òuturî1pÿìswovpïîïufècÿde"e÷wwuüko ysªnou"eÿougÿ
` ¨¨à1¨ªìw"1vıüuún³¸f u»¹»wõ›¨"      ¹mödovwûaveßşÿrxwicrwxoÿ_ûuroïd}½1âwewï}u3wd/›Y( ìbì¢ìb d 1ìo/*îºf ""ı1`  1ïorubléï¹ÿåy íûîsÿsiïwd0t g}oşnô ÿşûuûü3qv1ühõ"tûçu1ëlmenü êo÷ÿıctwîÿvun |fàënïínüæowdnouìcuvùwt}wöügu(" 0 şªìw±s` ¯kuåtîıùåübóî3}{îd?
ff`"¹3 f31u»¿;Xn u u ª¨ì 0¨"ëuwweşwßëÿåşnÿîpusìñskÿ{0=êsublíg_ãg{ß}çìı}wûßbgwg6±nslwîo»ß0q1±ª 1õä¹¹ let oïduus×uısÿeÿseûwätº¨ÿewwnõ¼Ögwxõ16="İícïvåErsoû"=ºbawî¾tÿuvgoîw(ùõêÿùï_ïıy_íşìwlwÿ_óïse¾¼.ënöe})©{
1»sîwuäş  ì¨nçu1ut¨vægñ=wrand»:tèrícd_r~ç;od3ù1"`æ1"f»¨muösy3odwäuw}dçcodgßríwuõü *¨d»0ª`¢±øªuq  à Oìríwuıt) ½z"{ÿM
î»ı0ìrì0ù¨ªwî1d±d±ì n
¢ ÿ0ù3 0fu1¨¨" ¨¹ 3ìï/ÿòıavuäqırniû³ûg}1wsom ÷ûoı moìuîÿw¨qîddezuonüntu¹î wnì¨wm"k~utıy÷ªïeÿü áüdi÷îtqsãgvbatøroÿsş¸uìıuïsíatig tàeìpıflmg óe}¸îrïtwï]vşúªÕwVg
00¨qıd±û"  ô ªdfÿ¨1 ÿgoüulus"x÷3ÿínı0uoÿwgóvevwÿsï}ugäxíntäéw3ôıwşfv rwêl{û_kuù_~nÿÿªrïûıestMŠu¹u d1têdşì0"w`ª÷¨0"o¿txp÷níîtwiw`sémw woşfeşïúù"îı~÷öw~ewfc|ignı`yww`o÷~ÿî õnuumvsf»n.uÿeséwsvwsuût÷uBûïİınÿ¢sıìunü"æo siçqäu¨ìéÿİxnü{:wúom*3o½1ÿös~îõnş ècısö~urí coæûtrıúvîì ÿsïç3êyöí arrc{§Fî"è¨õf t ìæ¨ `1¹øu¨fo?(/l1dı ±21¹3uq" "f`   ìîev2ïyÿ~neü_î{tesş"Ûÿ:»"uİqyd[3]0~n`ygUı÷v3dogûuîûısúoÿı skmple³îvomdénymove
 p`d qd"¸d»" "1qd¨11nev mdulõw:èûk÷Õnı"?"Fiïuiîvz:wvoo_êıtåw_sü|fşísw}v);¯ÿıªª1"¹ 03dq1 11  " bluu"u}ğïeÿvºtwmÿUxtd="ßégw{nü::fvomb}tes_bí|seıqïn÷nv_f}tuÿ)³]
¨¸ ¸ı    3 ¨3äª  1ªfüetursa_pıb_ëÿù_rçóunôş òsaş:ıòvors:úRgswìv}ÛwëĞurıyëOgyşìdsûù::vóupub|igÏåy~úæew.mõulus|fgyğîuv);'
•nu3¸ ì   1 d    " ¨uìmétcytrsa_rwb_{ey}úüóu}t Æ0 d qÿ¹î ¹`""  ùddfq0 01_z(wwéÿqus_ûí}+ =¿"óM
nd³v¨³1dw3 0d`»u¨"êw ¨¨`    ìlïödqõâîmr_útù_sycì}eÿïesíşdoo÷üúiîg: svrínïd?ìrùnäººüsregä_sï(9/ "dª"ª   11r" ¸"d "¨ì1 2d d ¢ "1®óaåüıe÷iwer¨®eìtèınımgriÿ)Mª1 d1d0èdw  f  ¨d`1 `d ì 2¨u"`0 .ıëûeıu319
tª fì¢ì   dd¢ê3 `0f ä2 1¨ìdff f êméø(sêts;:wroï)*±¨df0  " d¨   11``¹¨w d 2¨0 1 d æwonlícü(©³M/*¸u0" u "du¸ 1uw3¨  03ì"3¨df rurúíu_g}igîtæpıân{ãßkgygjeôìuîÿeßúwnuï|ßsôò{nw1=fpwsìmïßûşy_chalneîwt_vánuïï}würzg>glïîelë»ş^1ddä¨"1d"dì¨d01ª  ¨"`¨t1 "» ëuòÿgü_cõogæt.{s_ûusıycßïõ{_chóìneÿçe_sunü±}1trıg;/

 1 1f ª ı" à3ı   ä `  " d01¢ìgş¨voıecşypü_bõues: ¿{w}] =ªpubıkcÿëg{ÿcya}gngvrefom_wtvéîvjas_bytgw(ùÿínª 11 1u   d d ¨  dd` "d0b¨1" let òscÿgnór{øtúew÷ıt~"úÿc~ewsors:~ríwwlu<wgg=w8> µèrwûÿpwî×ûuy.gnsúùpu(fõut¹sîïl¨Pkcsÿw1½Enërypt¿ t~ßgngòùpÿ_bùugsmÿM;šı"d  "f f q¹ s¸ 3à  1 ¸11 01}aşúh¨rwéßwîcúyñôvesuı1şN ±1ı¨0¢" 3  "  ÷î¨`¸ ` "df ¹ ¹ d_ã9îytu÷ıtïÿwork_wmıñ+ª= {Í ä¹11 q ¨÷"ªd"   1d¨t"  f1d dbª º1" äägt³basu7t×rgsw}tÿuStòiÿgd½"fãsí¾4²:unóïîulêytew_üo}workwmõh+;L
f1¨dd±¨ ì `³d1" ¨ä    3  d¨ì ì  f¨ dûeîdßğÿsÿmï_key_shéllgïíuo_skgnıãîküÿş8current_gıiçnı=swåbsocoets¬bísg6ı_rgsuşu+; b1  f dud11 » 0 ì1¹3¨f  ùt1 0qºıNd 3"  ş` ììê21 ¨d`¨¢1  1 ¨  ìñ "Dwú(urúov) =z1{¯Î0 d" 1 3uª2  1dù 0  d f 0"¨d`±1ìº 1 pr}ntln1*êwóríncrûptseûuìuªísvû û}ª.1grvos:;_ » ä`"ıd ¨`  f¨1¸¨3" ¹¨f   "f¨"1ı/; 3 d  d  d"111¨3 ±`df¨ ìdfd"}¢"  ¹ ¹ ¹1  1¸fàì2ì¢"¨ d u…
0d"1 q è 3±t`  "÷ "ª`ª ıTrr8írroÿ8 =~ {İ9t ¨1"¨  d1ìª ª 1ì¨1f11d1   ìqrotn09"Ûw] ívrr1û}f,ÿvrvïr©;p`d1d"2ä"01u  31"dªw"¨»dÿî "3¹b1" ¨ñ  fª`î13d}_ ¨1`d"  ¨ü  0ùf }œ(d¨¸30"1f  d ²"  Túúş÷şroş© ?¾"ÿ_"¹ÿ0  "11"` `1æ º d"qskntôîu("ßu eööoò¨û}î¬1uúşïr:÷/*±dud031 æ ä" ¸ 3ª¹d¨""¨3¹0 0¹ı  ±ı111d}¨e}sgf
±dÿ1"d ¨  ¨twífsgïuÿs.geÿì&cìyeÿı_xwouwrëúníncÿÿûe*9;	N¨"è3 ¹0¹   1wgûÿcóuÿwnûeíïvt¹&cnåentiìı;Ü
 f¨¨ 1d1õ¹ddûÿéguw?vımwu(fg}iwt_mfm»Š¨ "dsd"dõ¿Š] »`d¨}îeîwg1ÿoŠs`v1 ì dtömÿüln#(fsìieît1ufoes¨nştfe|iûv"+»_1"±"}oìıEŠænfpsowvÿsşeëuivıußmeósıgí;glûuvéd:¹ÿ¿u¿>f  1"¸ f33 uì"3fªu¹d¨d01"   wgsÿc{evs:»7ïwvæHqsîMcø<u34,"vesğoîîír>?MŠ ¨ `¸»dfìì 1"¨ b »" ª ¹fî11¨ïií÷şó: 7ïuÿ1Hïsh]cv=w6ş? Ò}xwnv~,
" ì1u"r" 1¨`dªd" 1ı¨ì3ù3d¹¹"ãxqnÿgìsÿ0fmÿt¨Hêwl]cq|ıw¼¿"ghéîuä>.]ìd"ì" b f»df¨ì¨¹f f"31u1ì `¨1{conÿ; 7íuu¹jawèÍtñ|ıw4.3Mwî¾,/Štì""ıf¨f ¨u¸ ª`ªì    1ÿ0ıq"wüõïw3&}wtuHesyÌcı¼w2u®wvew¾.İ_¹ì3àd¨»1`1 »î31 ¨   ¸13d13 ¨ïiínıûtÿvüw]îcüï;"wíÿvdVïÿüÏnyíîvstosåwDíté~ìŠ»ì  1ùd1  d   1 î1¨ÿ 1d1¸3¨ ııûsaguvgxt:dSuvèwlŠìu  w`ì1d3 ä"  ¨  1d2"u ä¹ ¨sınder~ &ûüı:~s{në»;mtws~ºwşîuuw¼swúmîg>¡¹ûï*
¹"dflív¨tucûıütefßoïssëge; Suwíîvî½0ÿíüdítïÿîrı_ûíse¾7×g~ìßueóşûrtit(meswÿgvÿıïxu;;N{ªdª¨/ïreçwkÿevªdersûtvuìêmgwcìawq ggnüevªÿeÿîs3uo¹bwìşrio}ÿuM²¯_1ª0ldô"nÿonßûvrùng_ıÿ_ücswe;¨¾wvr õ¹dÿcşyütíu_mgswgïí~ésßwtû(©nüvéï_matwìwû¹séş::îwü;v+N3 "neö {ûïÿ_íessqïeºıwwûdu_jsî~:reûuıõswrfußêóon:;öùÿg>"yèsevdíjûo:ºwroí_stú9ûsof_ÿşwmnwßö_uarsím;*]* èfmgşsè îwÿ}euwsùïeq{n `  "  d_{wgîwe©d}>¹Œ
/N  ì f "¨¹  ªlgu1currıæõ_géínş~fostkonü&mwv Sl}eît?"9"cıiínwÿnfuwïuü*®ïuçõÿußiî)ÿNM_¨ 1uî d¨"1"dïùtcìugwsvwÿvwxvîv¸
à"»ä¹31d 11"¹¨`fßïe¨ù> û}*" ¨ " ¹1  3 1 w"somug}çuü© =~¹ûÍ3  ¨30¹ÿ¨  üsd3t0 ı kfwcnéetnyÿ_e~isüíwæ{/‚ 13±f "±ìdì 3fä  ì"¨ ÿ""if`c}keötmóıautyenıùcuteu"{‚"q"1u f "  q1w"d¨ìdfd¨¨¨ª¹33ôsïïess_gwtnwnşyëùtíî_oıwsïge;ïniı~v_ët®
 ¨ ¨¨01  dwì"1ì"  "d1u è1ìv11 wdª  uus`w0ªu¸ wÿ¨ ¹13¹"¹fd3wefsïckevw,] f1ììîı3d f¸f1uıu0ì ¨¹dè""  ª¨¹1 1 `û31u 0¹"ªı1¹¨dd31fb 1ªëîignvw®¯
 d¨ ¹"ì" f ¨b¨qdu¨   ì"b³  ¨ "ı d1 1è¹¨ªq " ¹äuuwd  1 »¨ı1w}cníìs®/ˆììf"¨ıdd ¨è¨ ÷3  uî 3¢ ìî"1¨ìu1"¹u d "ı  ¹¹d ""d" f¹u1fìæuísû5*qsfwwd1 u`q¨u"dıd"1 ¨ »ªd"1d1¨33 "ddd¨ 3"ddûubªî"t"0  w¹1wşéïw=N1¹±"¹ì dìª3"3"¨ d÷"t""1t20¹ wu±1ª  ªfõ¨»ì ı ª  ¨u "fuf¹"f"slignw_óuríıWdcwé.Š1   w" "1"ufì01ì " " ¹3   d                               value,
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
    let event_hub = simple_websockets::launch(websocket_port).expect("faiîefdto lksten ÿn púv 809¸")»
N  ¨"prynuì!l"[i] websÿëket weòver1runinwuon1poruu{ı"=`websoëket_povş);
ª"  trintnn!|"ÿë]¹éniticlüsetur fou");]
   
ğ ddnop1{Š   " ¨ 1//everytùmg sesvír êanfìew¨wubÿc{ítdeşenu, jínìle_mgûseges_frï_webrtc_uhreeì_qîd_cyeck_sìienısdis¨ruî* ı ìfª"1/ïTOÌ: uwt"hqndlg_}ussa÷es_fsí_wufsüc_ührgéìßaîıßgheck_wlients1in itw owî vread

 ¨dd 3¨ hadlu_ouûwagesßfrom_webstc_thvead_and_clıck_cìyíîts(&vhríad_messagkgßwhánnelr.1, fmwt wgsÿoëëeusì1fïÿu ûlouus, &}ÿt ûjqnnes,"fvyvgad_messcïéng_cycnngl?09;*
¨  ¸    }atch¨event_hub.ğoln_evetª+1{ ""dd11 d   Evgô:ºCneëv(snoent_idlwrísqÿnderı ?3{?_     ıd"      d1pryülne(fc"g}éent conîeëtíd witywid33ıbììslyenv_md©;M
  ¨ "dd¨¨       websoãoeus.énwerühcliïnüyf|¨rgsvoîder);* "  1¹¸  d1»   uítªouô siÿgìï_slieÿt» Clienü0=fÏ}õgnt~ºdeweıìt*+;M
 1"îdb"¹     ¨¨¹wingle_c}yev.ésßauthgÿôkcávef ? false;_  d"           3smngle_ãıiínt.c}mınt_xdd} clmeîu_éf;3/odatûcêanÿgl1is kfenü{fùgd¨bùdclient_édndısiged¨sjort}1tè}sìwiîn1sausu ürïıî}í. îénd owùgrdÿay vo kdetùf} dctacêanîílw lqtgr¿
1 1  ¨ f"     1¨single_ëloíu.ms_ïşmsvyg ?dtrue;M
è    "d f1ª "ì1 winÿìecliwt.}s_puêlmc_ke{ßëhanlungí_sentª= falsí;o
 d¹   d"d  1"d "}et¨ìïveüùou¾dchrno;~DateTùoí<glúonï~:wtc?¨}"cèrono::Uts:w(9
 1d 1 " ¨      0let ui}gwtaot: kv5 ı"watetkíe.timestéır();/1dd "ªd d ¸" dî¸wíng}í_c|ienü.wymgsvímp_cïnneïved = vkmgwtqmp;/         u 1 ä "s}nglu_gìkentn|écúopènestaşí1?f4;
 fª ¨ 3q     u dclmgÿös¿ynûewv(clmíntÿkì, sinÿ|ı_c|ient);/ 3 1¨ d1 ì  },/

1"  d¨ıd " uÍşgÿt;:Diûcoîîesv¨clientit_owßdmsûoecüud)f=>"{ª    "d    11"1  úsiwlî1(îClùgnü ïû}1dksãïnnïgteu.", c}ignt_oäïf_dmwcîngcteum;/
ª ¨  ¨  11  ""f¨ùroseÿs_cükuît_îksgoîuct(7íut3s}iets,¨1"¹    d ¨`1 ª1ı  d ¹ "1¨¨ ¨ª      ı 1f`1&muü chcne}û,   " " "  ¨   ""¹ t îªt1d 3``d1 "       d ®mut`wwfsoc{euw,/* "" ¨  ¨ª f "   dä îu¨ f ¨d  d±¨ 1` »ì 1  cligt_ku_ow_fióûînestgd,MN  111 11¨ı   d¨  d d   ¨  d¨1¹¨ d   ¨  "" &üyrecäıewségyîwßcjaînel.t+;/
  » 1 ¨ d¢11ı.

3 "   f¨  1 Event~;Mgûóage(ëligş_él, mussagu)d½~ 1 uª¨  »31   d uo/qûynwln!("Rwëukveu s íussage wvÿm clkenw #{}: {:?}î ë}éent_yı.ì}eûsaïw+;Š   11 1   ¨u 31 matgh }ısûawí"{M
"d   u»ut3¨ ¨¨  fd dMeûûawu::Ueøwlwebsoïkeüßmgwsqÿg)"}~d{¯
"d ¹¨ 1  0¨ 3  "   ì1  îprocesw_ûegemwgußmgsscïe(glùunvßmì,MN"ì   11udd   f»¨   u " 1d1  d&out wíbwockets="  "   ""¨uq"ì3d"¨ÿ¹ ì¹ 1ddw ®}uv cléevs=/;"    " ª1" f"¨3f3 1       f d¿mutìghùîínÿı
 "1d3f  1ª11"¨1   3è¨ı1f¨    wmıt¨kcns,MN31 1  3 1¹¨"ìu ddf  1 d 1  ""&mut¹vgïs,
  1df   d"  "d  "¨  3 è ì ª  &u|ìë}yent_ûuoríuÿdqvs®›d¨  "  3d¨¨   d "  1 ¨ 1 ìı  wífûg{evßmíssqge,]Î1  ¨fff  "¨1 u" `»ª" 0   d d ïthreqı_}esÿagùng_ënınnel.0ï?
 ¨  d¹d1d¨ 1 u" ¨ ì¨
 ¨wu"   f¨ddd"d!u   ß¨?> {}ª  ""d 1  ¨  d1 }d¹ f f   ìf1}=/+1  1"f¹"}M
ì   }
}M