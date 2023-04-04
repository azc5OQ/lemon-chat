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
    let mut json_message_object: serde_json::�ar|Strmng, se�de_{�on::W�lu�? =dse�fe_{s�;:Mcp::new()��

 u �jwo_messawu_osjecuk�suvt(sur�ng:;fro�("ty�g"�l serfe_s��:��elue::�rom("mkcwphoe_usag�"mm;
 1 ��sn_mgs�awe_�bject�insurwl�tr�ng~�fv�m9"�}iu�t_i�"m=ser�e�json~�velue~:f�mhclient_idim��   {son_}�sse�u_ob�ec�.kn�e�v�sur�ng�;fro("cyanngl_�d")�surfu�so~;�al�e:~���m���gnnel_d)m;
1   json}�swwge_object�msw�t(Surmg:;�vom("�alue"9ls�vde_jwon::Value~�frm���rophong�usage));M*
�u �json_r�ot_objeg�?�n��vu(�uv��w:�fv}9"mess�ge�+< sesfenwn::Velue;:��om9json_oessqgeob�ec�));  �dfor (_kgy�dcnment} �n1clyenvwd{M
    "1� �f1g}i�nt?kw_gxmstmg�?=�fals� {];03u     �d" continue;� dd d""}�M*     � �i� c|iew.is_e�t��ticaved`�=dfal�e{MN��          co�tiue�N "�`   �}**t� " f �mf"microrho��usew� ==uw�{�* �   �d�� 1d//�f�we wen�0to�s�nd m�fosm��ion e�ouv�wrg�king ���wh to`tc}{m1on�y�sent"��"c�kents in actuul gjannel
    3 d     �� �lienu~�hanel_id a=�sh�nnwl_id �� d1�f 1 " ""  �3g�nt�n�e;
  d   fd�3  ]
 � " "`u}�]*�d �"11 ?/yf�c����.c��ent_{�"=}�clign�id1{�
d  "   �o/��`dco~tin�e;  � f"  //��
   d � f�wu gur��nts}kent_wefsoc�e�: otto�<&�uston��r? = wubscket�.get(fc��env.slk�nt_kd�;

 � f  d�matgjdcusrun�_cl���v_��fs�goet1d  0 d�"� dd�on�d=?d{}�N""�""     d1wo}u8wuwsoc{et9 ?>�{

 0  d 1 1"wd "d lgt1j��_voov�obnect1:��cp<Svsyng,"�elue�"=d��o�_v�ubje�t?slong�);
�
  �"     "" "u 1leu"uest = serue_j�on;Vclue~��bjegu�j�on�soov_ob��svw);
 f� � 3q     u dnet"�cu�_c�nugnt:d�tr�n� = se�t�_jso�::to�tsi�(fue�t)?unw�aq*);M��d " u��"� 3   �ng��dqv�_to_send_waw�vt:�Wusmn�1} gncryp�_strkng_thwson�urwt�_b�se�v(�d��a_con��nt�M
u     1 "�  f 3�webwc{�t.senulMgs�a�e:�Teyu(dcvg��se�u_b�su�5)�;N 3�1   1�1 3}
 3d  �1�}/�    }�}
��� ms�mcy��a��er_of_�hqnnul_l�avinw_t}�t_shanngn(cha�nels:�&}u��]qwhap|u66, �hannel�, cliwnt_iu_uo_cjeck:��6���o>"(boo�,�u�4o �M
 d��lut ��t1resulv (bol}1w6��3� (wansel`0); 11��/che�nNN   �fov�9��e}. c�anngl)�yn c�s�nul� {�_�*" �  d dkw"cha�nul�iw�sy�nnel ?="falw� {
  d  d1 "�� con�inwu;]�    1"  }
M�d d 11 u�� c�snelniw_cy�nwl_maknw�yner_pse�unt == wv�e {*  3  " "    y�wc�u�nel.}�k�ua�ne�kd �= c}yet_i�_t_checo{_� � 1�  d "d   u�uww�t�� =ftrue;�� 3u  1 �3d   1 �re�}lvn11�"cjenne�.cla��e}_id3;      �1  �     bve�kN1"1  """"�d }M�1 "�d� d}/�    }]NM
  f��etuwn"ruwult/*}
�fn find_wwmakntakn�w_f�s_c���n��(slmwn�s:f&ou�d{csyosr<u64. �}kenv�?fshqnnels: fmu�dHe�nM��=�6��1Chcnwn>,1�u_o�_cliw���un�t_diwc�n�gcwgf:du?4,1chsn���u_tofid~ u64."mind�vn�_clienu_vhat_nefv_dis�onne�t�d� bol)�1(boo�="u�u� {/;���d�et"muw`��sult:"(bo�,1wv�) =�l�slsen�0�;
 d�1//�vdto"�ynu"ne��oaintamner�N
 1� yf �mn�_vh��lienv_v}�t_�eft�dyw�onuct�d�1tr�wu{
 f�ddd"dfr (��gy, sli�nt+"mn1cl�ents 
d� f f   �d1/?kf1clygn�"hes�same�cyenne}_{d cw �loent�thau"ks leeving g�u ovfcuv��nuclye�u"d u d 1 m�"slignw?gl�nnul_i�f=="cyann�l_id��o_��nw"&f��u�of_cloew�wa�_u�sconnec�edd!=d�lien�nclyentid�{� b d 1     0u�11rgwul�?0 �"�r�g;   d/?fo��f tje1mai�ekner����   ���3�  �1" resulun� = ��ie�t.sly�nu_iw;11�ogli�n�_{�cligv_mddof n��d�eintei�r
�3 dd    u  "   fr�wk;
�  1 d �1d� ; 1� " ff}/�  d3}u��we1{M
q u "�� f�r�(_e{?�c�i�n�� i�siets �?
  ` 1""  ud �?i�clmenudyaw1s�m��hqnnu}�id1e��clienv thqu ms lu�vog1qnd �tdswsrenu_c}kunu�
d"  "1 �1  "kfbc�meunch�n�l��f=�cla��el�d�o_�in�"M
1 �    �"d1ud�  veww}u?0 = true  "d?/f�wnd thed}ci�twynws
 d1 d�� d 113""dresultn1 ="sl�enu.cnignt�i�;" ��slient_y�/cli�nt_yf"f�ne�3}ain�a���w�d0d �     " � �!brgu{�
 �1 1�  � � }N1� " �"113  }� d  //�f�we fowddthe1��it�ier/
 d �yf �ew�lt.t =dtrwgd{N�  �1� ��vu(_�ey,1chenn�l)dmndcyan�el�1M� " �� "d    �w}cyannel_id_��_fm�d ==dchcnng�c�an�}_{dd� "d1fd �  d�d d�w�gnnelnme{�tcy�ev���=�r}sult�u;d/osetd��inua��r"m�
 1  � w    �d3 wc�ann�n.k�_cnanemaint�{nuw_pvuw�nt1=1uvwg?
 u� 1 """u  �Md 9�  �?�u �"ense {

� �� f 1nu� aa"? c�g~ne}s.g�t_uv}&channul_����_�y�d+�*�    """m�vchugad{MN   1 �u�d"1 Nonud}> {3d " "f    �� ""�s�ntl�(�#!!!�c��o�"S�U ms_slanne}_m�{nwain�s_��us�nv uo falwe?�GANNOT FMnD CHANNE]e#!!3);/ d�1 "d"    }N1  �"  w  ""som�(v�lue}d=~ �
 dd3 dd� 31d  �����u.�aitqynus_if ?`1;
  �1 � �d  �  1 vqlu�nis_�jannelmqintai�er_�r�wwnv } fqlse;�  ��    1 � `�  p�o�tl~!("setti���ys_chcn�el_m�inteinerpres�nu�vo fa�s�dfor`gla��elf{}" , veluen�ame.clne9));
 1�  "      }�       d}
    }�
�
 0d retwrnqsesult;
}_Nfn suuc�oss_t�reeu_meswagu_��yunt��msco�negt(senf�s:dfstd~:sy�c�~m�sc::Se�dev<Str�ng>, clyunt_i� u7��d{/
d � �et out�n�on_r�ot�o�{ect~ sgrdu{son�:Map<Surin�.dsg��u_jwon::Va}ue> ��sgrfe_��o�~��a�::ne�(m;�
N d �nwon_ro�t_bjec�.k�sgrt(S��yn�:;ws�ml�t}�e�), serfg_json~:Va�ue~~fvom(��lignt_dksc�nnec�fo);
 d� js_r�ow_obj�ct?insevtnS�r�ng:~fro�}"clxent_�d"}n3�erdg_js�n~:Valwe;~fvm*�lientyd)+;*o*d � l�t��etq_contgt; Svrk�d��s�rde_json�:t_svr�ngn�jsnroo�_o���ct).u�wserl�M
*  � senuer�s�nd�fa�a_sonvenu)uxqecv}�";
�*�fn sun�_crossuhr�ad_mtss�gucru�te_new_clienu_av��vc�thr�ad�sender: 7svu::sync:�ops�::�edus<String>, clment_md: u64)"�M�d�� lut0}ut nson�root_obnec�: serdejs�n~;Mc�<Stron�,ds��fe_jsn::vel�e> = sgvdw_nson�:M�s::�e�9);/n
� 1 nsn_s�ot�obje�t.�ns�ru(S�rin�::fr�m�"v�qe"m�`ser�g_json:~Vq�ug::from9"c}yent_c��nect"9;��    nso_�ov_ob{�ct.knsert(St��n�::froo(�c}iet_id")} s�rdejson�;v�lue;:fr�m9c}i�v_if));N�N   "|etdd�t�_contenw: Stsingf="se�ue_jsn~�vost�ing(&j��_�oot_o�jewt).uwvap(m;/N 1� �enues.send(data_co�tent).e|�ect�"�)�
}
Nfn sent��rss_vhrg�f_�g���ge_sdp_aw�erl�uder: &wtd:�s}nc:�mpsc;�Sendur>Strmng?l1sl{ent�yd;�w64, valug� werdg_j�on��valu�m �/
  � l�u ouv jso�_voot_s�ect~"serue�jsoz�Map<Wtving� sus�e_json~~Value�1="serde�jwon:�]aq��ew�)�
" "d{son_r�ot_owject.insu��;St��ng::fv�o9ft{pw"i, werde_{s�~:;Value�:fvom("sdp�a�s�er"9)
 �""json_root_bze�t?ynserv;Sv�ing�:from*"c�ienv_{u3m, sg�de_j��n�:W��ug;:fsom(�}ient_id9);
  f {so�_voov_ofj�ct?inwuvv*Stryg:;wroon"�sl�e3�,"wqnug)�
n  d net�uatq_cont�nt: �vryng`��serue_jwon::to��smng��js�n_vo�_objesu+.unwva�(m;/
  � se�uer.wend��qt�_c���ent+.u|uecu("TODo: psni� m�ss�g�");�}

f~dsed_�rss_thrue�_oes�age_k�gce�d�dave;sen�ur;�&swd;:syn�::mvsc::Sgndev<s�ring�, c}ie��_ku:�uv5,�velug: serde_nwo�::v�luum1�NM�3� `|eud�utdjson_rt_obj�c�� sesde_j��:�Ma�<�trinw�fsurfu_�son::V�lue>d= se�du_jso�~�av:~n�w()�N*1   j�_roov_be�t.�nsurt�W�ring:~�som*3uyref)���u�dujsn�:vq~uu�:wvo}9bige_candifq�wwrom_sl�env"))�*   1json_root_�bjgg�.�ns��t,S�ryn�::fsom*"�lke��{d"9� sesdejs�n::Vc}we�;fromgnmentid9};/*    n�on_s����sju�t.ins�rt(Stving::�wom*3v�~u�"),�w�lug)�NN  " n�tfde��_conveu� Stskng �1su�du_json:~uo�strynw9&json_�oou�fj�s�).u�wrap();�*w u�sudev.wend(dc��_convent).�}��ctl"�unu_��oss_thr�af_mwssewe�isg�c�n�idcte�xgupv"i/��
��n sedcvosw�t���ad_message_cha�ne�_{oy�*sedus:"6svf::s}ns~�mpw㺺Wenue�=Surmng>, c}ignu_i�: ww4,`chan��l_id: ��6)d/
�(��0 �v3�u�djson_ro�ofn�c�:1s�v�e_j�on;:�at<Stw}�gldse�dej�o�:Va}u�>1}�serde�son�:Map~:newl�;/ d31{wo�root_obnwc�.knseru�Svr��g::fro}9"tyte").��evfe{s�n;�Valwu�;fr�m("channe�_n��n")+;�0 � {wo_rot_o�jgct.mnsuvt(sur�ng�:�vo�(�glm��t_id3�="�ww��_j��:vcue::fromlc�e�t_iu))
�1"�jsonr�oto�nwwu.ynsu��(Strin�~from�"gla�ne�yd"9,d�erd�_�son:��uluu:~wrm(c�enng}_i�o)�
�
dw1 leubda�a_content: Stsingf�"serde�n�on~:t�_stri�g(�json_roo�_osjw��m�unwrap(m�

 1d1sendgr.s�n�(t�ta_so�uen�).eyqest�"�)M_�
nfn�broa��ast_clkenudisconnec�(cl�ews: 7mut Hg��]a��u64,"cni�nt>�1webscgts:17LqsyM��<u6���Sgspod�w�, �mgnt_mdof_fisgonn�gted:�u75o "{
" " �et cnment�p�kon: Ottion<&Clienu~ ��clienvs.ggt(&wlkent_ydof_dmwc�����uwd9��
*d"" mstgy cli�nv�or�io�{ "d"d�  Non� =? {}M�" �"  "dSm�9�nyenv_�hat_so~ngs�ewo =� {�

`    "�    dlut out���on_r�ot_wjec�:dsgrde���n;:M�r=Str�n�}1��rdw_jso:��anug~�� se�dunson:�oap~:�ew(m;;  � "�    0���� mwt1nsn_mess��u_osjgc�:"�g�dej��n~:M��<Wuri�,3se��g_j�:�Valug = serdg_jso�~;Met�nuw�9;/?
wtd1 u 0  " {wo_me�segu_�{ucvnmnserv(W�wk��:~froml2u{��m=0surtg_�son�;vcl�e~fvoo9&clye~t_uksconn���"�)�
�  "f  � d 0json|eswa�u_obn�ct�insevv(sur}ng:;�rom("sient_id"),serde_json::Value::from(client_that_connected.client_id));

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
                    Some(websocket) �>"
N"     1   1�"   1�  �1  let�jso_s�t_objectu: ]�p<Stwingl"Waluu> =ujsn_rv�osnecv?c�one9);Mn
  �"  `    "  d"�� � d dlgtltest"="susde_son::Value:;Ob{e��({sonr�ou_object19;o
  1d�  �  f� d� 1 �1 1�dle� dqta�o�tent~�string =�serde_jso::to�string9&tesu�.un�sep(�;� " � �f�"1"� d�3  u"   1let"dq�a_t_senu_basuv5� W�rin悔 ���vypt_svring_txen_convgr�_v_fase64( �eta�ontent9;/�d    �     1` !d d  d  websocke�.sen�9��ssagg::�ext(data_v_�end_base65))*  df3� ��d1 �1d�u �}o11    1�  �"    /
"df1d"  d   }��  1 1" }/
3 1 }
}�
�fn bsoadc�wt_clienv_}werneoe_c�cge9cnkenus:d&Hashoqt<�65l C�oe~v~, webwowo�vs� &Ha�hous<ww4, Re�ponder>,dsliu�_md:dw649�/

  � �e� cliew_osum�n��_ption<fG�ient>�="gl��nts.get(&climnt_id�;M
/�    oatch"g�ignv_�ttkon {*  1��  d_�ne }�{�/
    �d��Sme(�l�ent_wh�_cnewgd_m�s_e��)"?>d{/�`""1     ���let mut {sor�o�_objg�t: sgrfe_json::Maq<String, serde_{son::Val�e�d= suv�e_json~:Map::e�*);N   � " `1�f }ev owt �so�m�s�ege_obnect>`�wrdg_js�n::oau=S�wk�g,�susdu_{s�n���q�we��= ses�g_n�on::Mar::e�(m;*
 �  �� d    js�n_muss�geobjecu�insert(�tv�w:~fr��l"vyqg"+l sevde_�son::Vanu�::�som("��iwnt_runa}e"))�N   d"��  " 1js�n_mgs�age_ob�egt.m�sert*Wtryng::wro��new�ws�vnamuf),dserde�j�on~�Va�ue;~fs�m*c�kgnt_who_whugetits_na}e.u�ername.clonu(9+*`  "`1 �3 djso�_oessage��bjesv?ynwer�lStr�ng~��v�mls�}kent_�d39,"sg�de_j�on:�value::fro}(cliunu_who_chanweu_mts_aoe.clignt_md�)3M
�  "   � 1"f json�o�tbjectinsevv�su�yn�:froml�me�s�ge"),�s�vde_json::Vauu�:fso�(json_�essage��fjgcu��;�
/
  �" �� 0f� for (_ke},q�lme�t91mn slignts {�

 "d    11"1  �1 {w �}i�nt�os�e�isuing"== �elsu {
 1 "f  f 3"   dw 1�dcontynug;�
�  �  11  ""f�1 
�  �"1�  �" d 3�1if s�yeu?iseut�u�uk�atedd=�`wa�s� {M
� "1�� ��      � 1f`sont�nue;
 u�  1   " " "  �
*"� d �`1d 3  d1lgt current�clienw_websosoev: ort�on���gsrondev>�}�wgb�ockeu�.wet��c}ient.g}ienu_mu9;
�;�  1 "  d   111 }q��h cu�rent_cli�nt_�u��ogket�{
"  �1  " #� u �  �u�non� =~ *   � 1 � d*11�"    3Some(w�bscket) }? {�
*     "1"t�    1"   1 d�dlet1{s��ro�w_objecuu:1�qr?String?�Wcluu>3=�{son_rot�object.slone*);
�w �u�     �"�u �  �3"  �let1uest =�wesue_json:�a�uu�:Ofjggt(jw�_r�o��obnect1)��
3u  1 �3d   1 �  �1d"f 1�gt"deta�cont��t; St�ig = se�ue_�son:>to_�trinw*7tesv+.�nwve�9);�N�
dd"�    1udd   d��   u "psmtln#*"dat�_gontent1}", &data�v�t+���ι 1ddq �1 f " �d3""1"3" let"d�uc_vo�sgd_base64:fSt�mng� ��s�{��_strynwthu�_so�wert���sg�e64(qd�t�cnvent;;  2 1��"�d ddf  1 d 1  "webso�kg�.send(]ewsage::Tg|t(f�tat�_�e�d_buse��}9;
d�1" �d� 1"3���d�  "};`��   d "  1 � 1}M�   �d�3 1df�}�* 1   d�1}�"fd }/�}_/
f��send_clien�_lisu�t_s�ng�ec�m�nt(cnig�s:�&mu�dYe�h]au>u�4�G}ie�v. rew�ondgv:u&Re��ondes, �urrgnt_cl�ent_uwurn�mg:fStr�nw)1{/
1"f1net �ut j�o_roou_bjgwt� werd�_jsn::Mat|string�1suvfg_{w��:;value�1=1serue_j���;:M�r;~ngw;9?M�"f1 }e�fmuv {son�mess��e_��jwct~��u�fe_json;:�sw<�tw�ng, ser�e_nson�:Val�e> = sevde�{�n~:Maq::new8}� "duluu m�v�j�on_clmgts��vra{~1Vec=�erde_����::M���s�ri�w. serde_n�on:��al�e>? =�vuc!{;�MN  d �os|�_ke{?"client)"��d�lignts1�/
 �3 dd  leu mwt son�e_cl�enu_ob�ug�:3�wru�_json::�ap|suw��w,1serue_js��::�a�ue?d?"�esd�_��on;�]sp:~new�;;
  1""  uwi�wl��clmenu_bus�.���est(Su�inw~��rom("wserqme"),1�evue_zson�;Vewu::fsom9c}ku�tnwsera�w.assvr�mm9;o �3 �d"��og�e_c��enw�os�eg�.i�sevt(S�ring;:fsm�"publks_{ey"), serdenson�:Value:~wro�(w}mwt.puslm��key?qsstrl))mM
  "1 �  singlg_g�i�nv_��{ect.ins�rt(S�rinw::wr�m(�s}ann�l���w�=dser�e_nson:�V�lue:w�om(�}iu�t.�h�nnul_�d+)�/" 113   syn�le_clk�n�_object.mnses��Sur�nw::from(�cli�nw�id�m,1werdgnson�:V�}��:��vm9�liet.gli�ntmdm);
�
 �1dd� "l�� out mi�spyone_state��u64�= clmenv.ok��o�hn�_swete?
"d1fd � if�mig��none_wtew� ?=�1f{��� �"1    �u1migropl��est�v� ?`�;
1  � w  }�d3 u �  w�ngn�_cniuntobject�{nsuvtlSt�inw:;wvoon3miw�oqhong_st�tuf)=��er�g{��:�vanue::wrom�m��ropyo�_stcte)�/
   � 13ds}nole_c}me����j�s�.k�sg�t(Strkng�:fro(fteg_ids"�}�wgsdejsoo::va}we::from(cl��nv.�q�mds�c�one(�+��/�"1�1 ` 1{won_c}ie�wsarr�y?��s{�sknwleclme�_objegt)o
    }M
N" `1jso_m�sscgg_object?i��grtStrkng:�fr�m("u}pg"�� sevwe_n��n;;val�����so�("slyunu_list"m9;
  �1j�o�_oe�sawe_b{�gt?i�sert(Svr}ng::f�oo(�c�mwnvsf),1serue�js��::Vau�:~�rom�n�on_clients_���qy));/
�   j�oo_message_ob�gs�nknse�u�Wtring~:��omn"}ocal_usern�me"),suvdu_jsoo:;�a}we::from9�urrent_glient_uw�r�amunas_svr(}9);
   1jsn_root_sne�t.kns�rtlStrig::wr��93me��ege"�. werfe_n�o~value:?nr�}(�son_me�sage_ob�ect));* 1 ��et3��wt"?ds�r�e_json�~�alue�:O�j�st(nwonrov_obj�cv);1"" �gt f��q_content:1Stri��  ser��_�w��:�tos�rm�(&tewt�nwnwvap�);
N � "�ev dat���o�swu�be�ev�:�String"? engr}rt_�trongthe��convgstto�base�tn data_�ontut)�N  � rustondev.�e�d(Messc�u::Tuxv(d�ta_v�senf_ba�ev4));/
}M
o_fn�gheckyf_wserncme�ch�n��_a}lowed*slyenv_kw�d��4�  client�: 7muv �cwn�cr=u64, �lm���>, gl�gtw��veuda�c: f}uv�V�c>�ni�ntSvoredUauq>= }gw�awe: 7�gr�e_jsn�;Vqlwu+1->(bol1{�_"  1�et clienv_thqt_w��vs�to_�ha�ge_usernqog; &Cnien�"=1�nk�nus.wet(&clment_md)nunwrap,);*M
�d�f��t uatutime:dc�rono::Detg�ime<clrono�:Uuc>�="chvo�o~:��c~;nw(+;/N �d let3vmmwswamp�no� i�t�= fcvut�mu.vy}esu�op�);
�
    �f1cl�ent�vhav�w�nv�_vo_�lqng�usernaoe?�ioesvamp_ususname_��anged1;�3�> timesuu�q_now {�
 "" 3   ret��n�false� 1 d}
 1u l�v ngw_de�yr�d_usern�mu:1S�wkng"= muw�ege["}eswa�e�]["new_wserqme".cw_�ur(+.wnw�cp*9.�o_stri��9)�
 �  f new_uesivef_uwes�a�e.yw_gmpty() == tr�e�{*  d�  d�r�t�rn falwg;
  1��/_
 "  k�fn�w���sired_usuv�aoe�}un() > ut {�
d   �d "ru�urn wanse;M
d1d1}+
 �1 w�v (_key,1c�ient)"in�cl��nts �M
�"1    �if�slket.us�rname ==�new_fesise�_usuv�ame N   �  11�   privln!9"�serame �aoen,�unu��1mn�c�ienus"/?*  u � d "u  retuvn1��se��n   "f�"1}; 3 `}N
   1w�s uate i� clkenv_st�r�d�datq 
"�""  d if datq.us�rqme =?3ng�_fe�ired_wse�namg {d   "�  d1�p�i��g93usern�}eduagn= etryfk"c�mensvor�d_uate3m;/
  1u  31"d"fr�wu�n1�ansu;* �1  f" *dd�1}
   (vetu�n urue��;}/M
fn ks_chc��wl_c��at�an}o�ud(cnyunt_tygt�cveaugs_c�ann�}: &mut ��ye�u,(��anngns~ufmwt LasjMap>w6�, �ja�n�l>,1�essage;d&suvd�_{son~:Vql�u+d-� boon�
d� �lev ut �usw�t:3bo}1=�vrug;

"   o/chgck �ooluow��"1 �oeu �atetkme: chron:�Dav�Ti}e=w�vo��:Utc?d�dglrono~:wuc:~no(��*d1d le��ui�es�a�t_now�fi74"= �atetim�.timestam�(9;
�d( ofdc�ken�_thcucre�vuw_chcnoe}n�io�svqmp�}�st_chg�elssected���1?"uomest�mv_oow�{"u1$ d  resunv1�wfqlse;
� df}
M
"   ?/fkrst che�k kf�j�on �mel�s1g�ist?** � "iw��wssq�e["}essag�"]["rqre�u_changl_id"]d=} fclse /N  1쨨3�srytln!(�fie}ddmgssa�wnpcse�w_�jan�el_if u�esdnov g�mwtf��M
f " 11� reswlt �1fq�se;
   d}�
"1dwyf"mesw�gg_f�gs�q�e"]�&channwl_cew� }? gelwed {*�u   f pvin�}��*"fi�}d1m��sqge.ch�nnelnamu �ous n�vlexkst�);
"  d"1��ru��nt =1fqlwe;M� ���}
    if me�sa�e[f}�ssawe#]["chcnnel_duwcryt���n"]w=�1fqlwe {
 1 ""1  �rinuln!("fi��d mess�og.gncn�g��eescr{�t�on does1n�v g|�s�"9;N�  "�"f1r�sult }u�elwe;�
 " �}o�/
 1d�iwd�gssag�_"me�segef][fsl�nn���pessworw�] == falsg�{/
   1� " ��invnn!}3fmeld mg��egg.cyc�nu�_pasw�vf doos nov �xm��");/
 �  � 1drusw}t = anse;M
 1d }�.� �di� �esw��e_"mu�sc�ww��"p�ww�tc{unnel_odf]?�s�m649) }= �qn�e1{*"�d    dswynwln1�"fieldf�ewwcge�peve�t_�hqnnul_�d"w��g"ty��w);
f3 1   lvesw}t �ffg�s�;M1   }o
�d1  of messagu["mg�sagef]�f�hanne�_name�].�s_string�md==3��l�e"{
  d�    psmtln!*"fiml� o�ssuwg�sha�nel_e}e�w�ongu�ypef);*   ��  dveswlv1? false;
�   �M?  1 ig mg��ug��"messcge�]["�yannel_ugscrystio"ݮis_���yng()3=�w�al�wd{M
 d   " "prmntl�e("fi�lw1oessawu.chcn�el_dgwcrmr�i�n"wrong type");
d1d ��  resw}t ? fa}sg;/
 1 3
Nf ����)}wss��e["mgssewgf]["ch�nn}l�aws�oru"nkw�strin�()1==df�ns�"{*d u �1�fqrint�n!lwwielf1meswag�.c�anen_pas�worfdwwong t��m"m;/�  uuu   �ewunv =�n�}su;�"1! }�*�_1 � iw1rgsun��=?dvv�o {� 1" " " �gt ms�_pavgnw_c�an�el_id�� �essawe[wmessag�]["qcvg�t�j�nngn��df].��_y74(}.uwrm�*9;�1 �d d "}et }wg_clcnnml_neou�=1S�sinw;:wwo}(}essage[foussa�gf]["s�sngl_namg"]n�w��rl).unwsqs��� 1 !"1� le�3osg�clwnngn_dessripuyon =$W���n�:�fr�m(ogssqge{#mewsswe#][*s}ann�n_�escrortknwnas�svr()?wnwrap());
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



fn is_public_key_challenge_response_val�d*messege: &s�rde�son:;�al�u) m> �ool1{�
d"  letdmu��resu}t: fol =1truu;"uu"�f1messqg�["musse�e"][�va�ue"] ?= fc��e��M
d " �   print}!("}s_publ�c_ogy_cya��ege_re�pnse_vali� oessqg�.t�g_o� d�es �t1�|ms�"m;
"� �  d v�sult } f�se;
 "d1   d�euurn sesulu�
d�1d}�
� " �f�o�ssc�e_�eswcgg"]["vclu�"] }=1fc}w� {N1� d�  f���n���g("is_vublic_{ey_chglne�ggresponse_�eliw�meswagu.c}�ent_i� does1noude|ist");
`   d� "ve�w�� = fcls�;
     d"1r�uu�n rewu}t;M
"  d?���d1 �ut�� �ewu}u;
}�
�n0is_swbnmg_ogy_mnfo_o��sawe_valkd;musscge� &�erde_json�~Value) /> foold{_ �"1leu owtdsusult: bol � uvwe�o
*f   mfdmws�aw�["me�scww"_"valu�f == false {� dd d""dq�mntln!(�i�_�ublic_eyyn�o��ussagg_ve�id me�scge��ag_id does n�t uxi�tf);�   �  "1re�untf=�fense;/
"  1��  vu�urnuw�s�nt;
 �d��M
 � �if mes�qgg_3�gsse�e"_��erkfmcauk�ns�ring"ݨ�} false"{N1f � �  prk�tnn!*"ks_public_kuy_mnfo_message_walid me��a�e.clye�t_iu does no�1e�ksw"m;
� " d1�fruswlv"= �sl�g�
�d     dretwv�3result�* " u}�]*�d �kw1mussa�e۪�u�sa��"]["w�nue�].is_�vrig�)d=="fal�e ��d      �psitl�!n"is_p}s�ic_ke}_�nf�}ewsa��_vclyf oessagent�g�id"msfo�t"�s_wtr��g3);:q  q    �ewultd?��alse;*�d  " � retur� vesu�t;N"d  }/N"u  ���"m��wq�e_"muwspge"][f�g�ifm�et�onwts�ng"�.ks_strmw(}1==0walsud{
" �     psintl!9"w_rublkg_k��info�eswage_v��kd 1ouswegg�glme�v_m�o� n�ukw_st�iw"+;/�    �   �gsult ?"fc}st;"1      retur �esu}t;
 d�"
M� �dr�uurn re��}v*}
o�f� {w_add_wemove�ny�t�tug_mew�eg��alid(�e�wage: &ser�ejs�n~;V�luu)"-� soold��N " u��v�mwt r�sw��:1f�ol = trut_�Nd �dywdm�swage["me�sage"]["taw_ku"]�==3wa�se�{M�b � �  1println�lfiw_add_seo�vec�ent_waw�mgssawe_va�i� f�elu1messg��/vqg�yd �ou�0n�v ex{�u");�1 311  3ves�}�1?�false;�N1 �1�  dr�tws�r��ult;
� 1f}
N�   ow mu�sawe["ogsscge�]["cn�ent��t3] ?=`fqlse {
  d �   pr{ntln1(3mscdfr��o���glkent_ta�_�ews�ge_v��iu`f��lu messgwe.c}ieui�w�es1nov e|isu39;��   d� result � fe�s��N   �  ""re�urn"rw�u}t��u�"�
d d"yf me�sqg�[f�us�cge"]{"vag_o�"]?is_u64l9 ?��fcls� {/"u�   `1rrinuln�l"fiu}du��ss�wu.tag_}d y�1nt u66";�*  1 3 �1result3}�fansg;;  " "   ru�wr�u�esulu�/�u � }�;/
 �  iw1mussa�u[3}essage"]�"�ly�nt_iff].iw�uv7�)��= felwe ��
3u  1 �3trinul�!(��mgnd1�gssege.�nken��iu is�nt w64"�;
�    ` fr�sult1?1�ansg;�N dd�1 "�v�tuvn�resu}uM
  f��
_ " 1vutuvn"resu�t*}Nw"isservg�_sg�wmn���a��_ewt�w_vanm�lgsscwg: &sgr�ujwo�;~wa}ue) -> foo�d{M�o
��1�n��1muv1vwwulu�1so�} = w���;o�
  u �f�oewwqge_3messawe3��"�}noef_ison_yd""== f�nw� {
  1 f   trinvln!*�fiwl� �e�sagu.c��el_m�1fo�w�nv3���w�");
3`��   drgsu}t�=1fe�se;�N�3 1df� �gturn w�sul�oN  "�}_/
`��"iw mewseg�_"mes�awe"�_"�ink�f�iconi�3�.i�_iv�l9d�=1�uns� ��n1  �wu" pro�tln#l"wiel��oessqge�linked_ic�n_mu }s n�tfiv4")�o""1  1"f�gsul� = f�lse?/
1 3  "w �evurn�resunv;
dd" 
o�
1d"fiwd��sscge["�usssgew]_"���_e�g3] ?=3wal�gf{ �f 1" 0prm�tln!��fi��d3oew��w�.tqg_na}w�we� �v exist�);M
 d�    �vesulv  f�}�u;o
d 1    `su�usn"vus�}t;�*� � }
N"3 "��fmeswqge[3�eswag���"ta���s�e"�?ks_strmg�) =��fa�sg1{� u  1"11�vmnt�n�(3��g_ncg msnt sv��n�f);MN 1�"0 �3rewunt } fclse��w 1  �  setu�d�es�t;�" ff}/�
d31u��u uag_a}e:��tr�n� }qm�s�a�e�"��ssq�u3}[vtag�amef]?cs_�uv(�?u��rep()?t_wus�n塀;
1�  yf��ag_naoenlu(m > 3� ~}duqg_n�eng()"= 11{*u� d"  "qr�tln#(fv�g_qoe �wn�tj��w1n�tda��oww� {� f��t�g_na}e�len8�+_d�  d d"susult =`wanse;M d�     return�rwsuu;
d1 d��
M
113"returndvesulv;�}
fn"iw�i�on_��}ad_mes�age_�aliu*ows�eg�;u&sg�d���w�~;Vq�uem -> �o�} {/_�   �}ew�mu� �esulu� bo�n1?duswe;
� d �f"�e�sage["}ewsagu��[wb��u64icon�val�e�� =�dfqnwe {/N" d�  �1��y��|!9�fie}d mes�aggnfesu64�ic�_v�lwe��ogw not�u�is�");
 dd�  p�esult ="fcn��;�
1"�1 3ddvw�urn1ves�lt�N d�u�M
/
 d d{� ou�sgg�۪m�s�age"�_3fawev4_��on_v��e"]�is_str�nw() =�dwas� {M�" "� "f urmvln!("�ssuwt_mso�_vqluudksn� su�i�"+;]
 �d1d ���us�nw1��wa�se;
  1   � ��twr"s�sulv;
�n2  1}
�
13d"}e� besuv4��r��g�1�tr�ng�= messcg�{"muwsegef]["sq�u�tycon_we�ue"]?sw_strn).un��ar*�?�o_sw�i�g();�*��"�"1�w fasuv4stryng�g()"�17��13�* 1  "  dps�ntln!l"bawe64_icon_vcluu is1to�1bkg");
  d1  �"rewulv"?df�ls�;
ud f �� retwrnd��swlt;�����}/�
3d11ruuurn result;�}��M*f� psoguss�veo�g_teg_frm_cli�nv(�l�etstorud_dqt�: ��ut Vwc�Cl�ent�v�retData>, c���ts: &m�t Ha�hMat<u64. Clk�nu�n"web��oets~ffm�� HgsyMap<uv4, V�spondes~,1tags: 7�ut"HashMcp=�64, Tag~, messaw�:�&suvde_{son;Value, sliunt_id:0u7v)�{*
�  d}et yw_val��;1bo��d? i�add_remo�w_gnkent_vqg�us�age_va�idn&}es�ege�;

" 1 �� is��elkdd=� �alsg
�d� {M
�  � �3 return; 1 "}�

  11ngt�kw_c��yn:dboon =1is_c��entad��n�g��e�ts. glmu�t_mdm;�No
 d  �f ys_ad�in�=? fal��1{�
311� d� t�i�tln!l"cnient ms n�t gfmon")��
 "`   " rmturn�Mn    }M�
 10 l�twclie�t_yd_to_rgm�v�_tag_fr�}:"u74"=d�eswcw�["mgssa�ef]["cniunt}d"�nas_u65().wnwvct(�;
 ��dlut tag{d; u64"?�m��s�ge["mews�ge3]["t�g_o�">as_u6�lm���wrapl);N/N�d |gt �nienu_o�t�on�"O�tin>&muu"W}iunt>u� slie�s.�et_muv�7c|igv_id�to_rumo�e_vag_�rom);

 d 1mav��"c�ien�_o�tion {
1" 1    Non�"=?�{�
1 !3   "Some(clmenv) }> {
  "d �d�d�� lut1uag_opti�n:dOption>�mut Teg> ?�vews.�et_mwv�&tc�_id);* "d �`    3"mawcn ta�_o�wio�d�
 "f1 � 1 f1   1�no�e =>�{}
 � 1  �   �"  "�S�mg(_vag�`=> �/
"f f 1  "      1  11// ��     13� �    d  ?�wdglie� unfes specm��e� clkent�d1e|ysus au t�g underd�qg�iviud t�g1iu �ksts
 1d�`   "1d d � �1  ooadw taw1id vo"ve�1if"iwsd�ot"ql�eady t��se�
  1 �  w    0dt   d /?N� �  1d0"  d        i� gligntn�ag_�d�.�ontainw*&�ag_y��"}= fclsg��"���        1d�  d�11   println�(fno �eg"t�teleue"9;N d1d11 2   �1 3�f       sg�uvn;*  �  �"    d �"1   }�
�1 " w    � d    11�dclmgnt.ua�_ids.�etain(|&x|�x 1=�tag_id}
 �1 "3    � d   �1 1�� � �   1" "d3"  u � o/kw trying u�sem��g adoo�"ueg? whe one wity1�w:10,dse� iwadoin �o�f�lse
f   �""  d       0 0/�;N   "3 "� " � "  d"  �f tcgyud== 2�{0� � 1�0d1 d1   �1 d` wliuntis_gd}kn }dfcnsg�  1  d1d""   1u  30"}/n�3d� 1� "31" " �1  f lgt"wt�uus; bool�} is�ruslic��wyqvesenvin_c�ownt_��or�f_dqt�}clig~ustosgd�deta="cli�nt.�ublig_ke��sl�e(�;N/*1dud 1  d  "   "" � i�"s�a�us =� walse 
N � 1" `d  1d�1"d �    "�vrin�l�!("pwbli�1kw� { ou1p�gsent, thct slowlf no� be1po��yb}e�!Mw �tw rgally notdur�sen�, uluw�dm�3�othig�toddofdl s}ienv.�u��ic_{eync��e�))��M
 "1�f 1 "1 � dd   � } else {�
1    1�d  f d �"  �1d "1prin�le("pwbliwd�e{�{0pr�w�nt " n sligu.rufl���{�y?cloe�)o;/
]�"1`"u1` d  "    f1�w"1 "leu �tevus1~ bool1= ks_tag_i�_pre�e�t_i�_gl�ev�stosgdd�tc(s��wnt_�torud_fat�. clkunt�qubliwoey.cnonu})."tag_mfm;쨨3�3 11     �"  1 d " if�wtavuw�"=�"tr�e {*�  d" " "�df d��d f " 11�   qrint�!9�is_tag_id_p�esgt_ynclie�twv�ve�_�ava�== false39;
]� q3  d u d   "�u   f  d "�u?�/
d �1 1 �� 1     � "  3  d1 �/?vhe�gdis"Cl�enuSvorefU��a1��vry wiuhdclm�n���puslic key, bu� d�esd�v cntqin tcg idM
1d  0d ��d  u �1 1 d      1 //qdd�tag1id to i��N     �#" df  � ��0 2 "3� �d //
 �" fd�"� 1  d�  "�"f1 � d  luu�ottio�_cni�nv�vorud�aue�? get�wlie�t_stored_d�tc���_tws}mc�ey(clienv�tored_u�tc,��lignt.uublig_ke{��lone(9+��
  "u�1d" d "      � d�("  " m�tc� ttyon_clie�tsvoreddqta {
� "�  d � �` d�� d  1�1"�3u��  �_�ef??u{}
 f d 3� dd  1  d  �1"� 1  " �d  Somw}waug9�=> {
f�dwd"" � dd (  � 1  1 d�d" �1� "  ��uluu.vsg_idsnvetcyn(�fx~"x�!}wuag_yf);�1  f    "  d1 "df""   d �d�  `  � " pr�nt�n!("reoo�ed0ts��". vag_m�);
 1d1    " "   �  �  u1"�1  �  d1}M � �  du�   f   "   ��  dd  M*1"    " "  �   "d131  0 }
 f��u ��    """ �   �
     u"   03d "1 � br���scst_se��e_�sgfrom_clkent(glme�ts, w�fsgkgts=1cnign�_md_vo_reo�v�vag_fvom, tag_idm;M
��      1  "   u}/
"  1 3"  `df}��(�013 1��"d""}M;o

�n"�ro�ews�adu_veg_�o_cli�nt9clme�v_�vorgd_ua�q�f7muvd�ec|WienvStosgdD�tc�,1clients� &mwt ]asnMe��u6t,"�liuu>, �efsocke��1&}ut�jqshM�r�u7<� Rwsronug��,"tcg� &m�t1jashoa�>u6t,�Tag~. es�ag�: &se��e�json;~Vulue, c�et_kf:�u6�+�Mn/��dd l�� }s_vuliw: n�o} �1ys�edd_rumovu_clkent_tew_mgw�awu�wanyw(w}essugu);/
N"1  i�"ms_vq�{u ?}dfalsgN�u"��/N    f 13���us;
 "1�}
�;"" �lew msqdmi: bn = mw���i�n�_a�minnclyenvs, gl{unt_i�9*M�" � if os_cum}nd==�fclse1{
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
  � "d  M*    }�
}�*fn s�nd�uirect�chau_q�sture_meteda��(clitn�s~"7HasyMaq}u65, c}}g�t?, wgss�ckeus:d�Hash�cp<u64, resto��e�>l wend�r_id: w6u= reweiver_id~dw74)1{�� 1  lgt�mut json_rov_ob{e�t:�sev�e_�sn�;Mq�|Wt�inw, s�r�e_nso�:~Value>�? serdenson:~�aq::new();�  d�ut m�t �son�m�w�cwg�of�wct"sgrue_nso;:Mau<Stryg, ses�e_�son���a���~ = sevde_jso::Map~:ne�(oN
    l�t rw�eivur_webs�gket � websgkttsnget(&receivev�ifm.���ap();*�   let senue�_c�ient =1clmentsngw�(��ed�s_��}.�nwsqq();�
 �"0jsomgwwqgg_ofject.��sest9stvknw:;from(�ty�e"),1surd�_json::Vcl�e::fvom}"�ksecu_cjet_qigture_otta�aue2)�"f  jwon_e�sq�e_ob�ecu?iwert(S�vkng::fro}("qy�tuve_kff9�dserde_�s�n�:Valuu:fsm0C��U_MESsAGE�ID.lo�d*Ov��ring::SeqCst)m);
� d j�on_m�sscgg_�bjegt�mnsert*Svri��:fv�("su�e�u�ern�m�)=dse�d�_json:�Vanuu:fro�(swn��rclmenu.�wgr�ame.c���e()))11d1ns�n�messc��object.insert(Suring::from("senwev_id"),�s�vde_{w�n::Walue::fr�}(�gnwev_id�);N�f 1 json_r�t�of�e�t.insevt(Svv�g::wro�("mgswqg�w+�ds�ruu_{sn�:���u�::��om(js�n_m�ssage_objuc�)m;*
 �  ��tdtest =�surue_�son::Va�u�::Objec�(n�_voo��objesv);M
  d le� �atc_co�en�; Wtr��g3= swwde_json�:to_str��g(&tusv�nunwr�p();
�
f  d�et dcta_uo_s�d_���g6��fS�ving1} tncrypt�s�rmn�_t�engo�erv�vo_basev5 uqta_wontunt9;/
�   receiver_wubwocket.sed��wswagg�:vext(da��to_sud_bc�e6t)�;N��n�
f�wgd_d�rwstcj�t_pi�tur�*clienvs:"wHpsh]cq<u64, Cliunt>l wuwsocket�: &Hes�M�t<�74,"Res��fgr>= mg�s�gw_value Wtrmn�.1�un�ewid:d�v4�"�eceiv�r�md: u64) {
3 1�leu �ut1jso�_soot_��nect��g�dw_js�n:��aq~�tving, sewdu�nwo�~;wel�u~  sgrd�_json~:Map;;nuw(�;;3 �1l�td�wt�j�on_mgssage�ofjuct: surfo_jwo2:MatSu�mng, serfe�j�on�:Vq}ue>"��erue�{so�:;�pp�:new;�;
�1 3}ut sece�w�s�ebsogk�t1=�w�bsoc�evs��e��&recei�esmu).u�wrar9);]�  1 lev sende�_clkg�td=��|yenwsngut*&sender_i�).unwsep();M
"" "js�n_���wage_ofne�t�ins�vt(W��ig:��rm("tyvu").1sewug_���::Wanue~~fr}9"uy��ct_g�at_pmctu�e"m�;�� d  j�onoes�qge�r�ewt�i�u�v*S�ring~:wrom(�pyc�uv�_y�"), swrfe_jw�n:;Value:~wro� CjAT�MEsScG�_ID.}oad(_rd�ving;;Su��st�;9;
d udjs�_essage�sjecu.{n�urt(Str{n�::from(3sendgr_usu�a�u�), su�f�_j�on�;val�e::wso}(se�ues_client.u�e�nq�g.glone()}�M;� ��json_mes��gw_ob{e�wninsur�(S�smng:;�roolfwe�dgv_��"9, s�rwe_json�;Va�ue::from�sendur_id++;/�d dd�son�m�sweg�_objuwtninsg��(Stwing;~wrom*"valu�"m,"sevdu{son::Vanw�?f�m*���s��e_veu�9)*�d3""{sn_roov_�sjegv�{nsest(String:~�vom�fm��s�g��9, suvvg_js�;:�qlue;���o�json_m�s�cgwobnest)9;:��"�letdvesu } serfg_jso�:~�alue::_bnectljson_roo�_ocj�c�)�
 u  ��u1date�son�e�t;"s���n� ="sesd��json::to_s�ryng�&te�t�?uwv�p�+;

d�1 l�vfdatc�uo_sen��base64~ Wt�mng =�ecr�pt�svr�n�_then_g�ve�t_t�_se�e7<}"d�t��gnt�u+;
f�ddvgge}ver��gbsoske�.sgnd(]es�age;:Uuxt�dgtg_to�wunubqse76o9;N}�

f� senf_uisecvc�av_oe�sagu*cnients:"&Hg�yMqp>w65l��lygnt?,�wessocets�*�Yaw�oqt<w67= V�svnue�~,1oessag�_val��: ��r{ngl��u�fes_if:1w�7 �ew�kver_id� uv4,d�erve�_chqtmgsw�w�_if: usize) {]�11 "lut0}ut�j�o�root_osjg�~~ sgwue_js�n:~Ma����rin��s�rd�_json::Vql�e> ��se�de_js�n:Mqr;;�ew()� 3m�let"wtdjwon_o��w�geofju�v; �wrde_jso::oap<wt�nw, �erue_j�n�:w�uu� ? wgrf�_jw��up;:new(};/��
 � �leudrg�e{v�r��ebs�s{etf= w�ssocoeus.g�ul&�uc��vev_ium?uws�p���  11�et1w��der_cnienu1=`cliu�tw?gut(&s�desyd).unwscq*}�N/
 "1 �sonoews�ge_fne�w.�ns��v9w�rmn��:fs�m;�t}��"�.dwerd�_jso�:~Wul�e:~fvo}93direct_cnatmusw�ge"));N1" �jwn}essewe_��jecw?{nsert(Stvmng::wr�m(3vanug"m� �erd��{son::Vq�ue::�rom9ogss�ge�wulug�);��w�1d{wo�_messcg�_�bjecu?�nse�u(S�ri�g�:fso}�"se�fur_uswrna}}"�,ds�rf�_�son::velwe:>w��m}s�nuerclme�tnu�ew�amenc}one(++m;N�  �{��o��ewsq�e_osjegt.�nsgvtlSuri�g:�wvo�("s��dgv_id"�=usurfe_json~��alu�::from(senf��_�d9+
3dddn�on_}gws�ge_�fje�u�onsert(Ww�inw�:fr���"�gsver_�}qt_mewse��_ku"��sgv�e_json�:walue�~wrm�sev�gr�hcv_}gwsgge_id�;;_M
Md�  {s_roo�_ow�est.knwer�lStr�n�::�r}�}e�sage"),1ser�e��son;:W�lue::fr�o*jsn_m�sssgg_objegt9m;ݪ�� �1�gv�tg�t = sgrd�jsn:~Velue:;O��gv9json_vootos{ecv);O
   ��ev"�q�c_c�t�nt: �v��n�"=�sevdu_nson:;to�svsing*7u��u;�unwsap*);M
�d   let dgta_to_wedbase66; W�sing"= encr}qt��vrig_vjgn_�on�ert_tobk�e6tl3det��cven�����1"� rwguyvus_wgbsos{et.s�d�O�wsa�e:;Teyt9�ats_�o_send_base64)�;/
��M;fn wenu_chqn�el��hat_qi�tu�e(c�o�nts: &HashM��<u64, cl�ent>� wefsockgts:"�jq�noap<�7�ldRewvon��r>n"}essage_val�e: Stryng= sendes�id: u64,"cy�nnel_id~ u64) {]� �  }et muv jwn_root_objuct: sesuuj�on::M�p<Wurinwl ser�o_{so��~val�g>d= serf�jwon::Mar;:n�w();
   llev }uv�nso�_messcgu_��jes��dsgrde�j�on::Ma�|�trmn�, �e�we_nwon::Vqnue> � serdu_jso�:~Mc��;ngw(m;/
    ��tfsgnd��_�l��n� ="clievs.get(�wgnder_�d)?unwr�p*�?/

 ��1j�o_}�sw�gg�o�ject.insertlStrin�~:fvoo("t��e"+, serde_json:~value:~�rom93ch�nel_c�at_pmcturg"�)�
 d  j�nmussag�_ofnw�t.insgr�lWtring:;gro}("pmcturg_id"+,"wgvd�_jso��~Vqlue::fso}(CjAv�E��A�E_ID.loa�(Osderi�g~~�gsCsv)))���� json_ouwse��_osjec�.inwurv�S�ri�g:�fro*"usurqmu").u�esde_{�on�:Valug�;froo9sundmr_slyen�.user�me.clone()m);
"��"j�on_�es�age_objesv.ynsert(S�ri�::�rm(3channel_id"ml werde_json::Va�u�~��ro}(s}annel_i�))
  ` j�on_mewscgg�of{ec�.insgr�(Wv�ing::wsoo("va�ue"),3serwejso�::�wlu�~�wroon}e�sqgg_valu�+)�

�   js�n_ro�t_o�jecv�i�sgru(St�mg:�fvom*fmgssagg"), sesde_{son>:��lue::ws�m�json_meswaweobju�t)+;;
   d��r�(_kg}n �lyenu)1in c}ie�vs {
N�1 � d 1if �lyet�s_ezistyn�d== fqlse ��1  dd  1   usontonwe;�  " w d�*
�      ��w �liet�isauthenticatef1=} w�l�e{N!"  d       co�tmnug;�  d� � �
  d"    iw��n}ent.chc�ne���!= channul�idd�      dd  � gont�nug;�1   1 "1}NN1d11 "  i�1c�gnt.cliev�idd=="se�de��kd {N �"1    �  �sontnue;�
d    11�}
M*   1 �  lut�current_cl�enu_�ebsockev> Oqt�n>7Resp�nder>�= w��soc�e�s.guv(&gkenu.�lmenu_id);.��3  ��fmatcn�cuvrwnw_client_wess�ckut 
�  d"  "   �N�n� =>1{
 �""  d     So}e(w�bsgket+3=>�{/
�
"  d"  �   " 111let j�on_r��t�os��cv1:d]ap<�urmng} Valuw> =fjson_voovofne�t.slonul+;
u  3 "d"f �3d� 1�ev3ugsv �1sevfejson�;Va}ue::Ojnect�jsn_r��w_osnect1+;
 "�f3d  ��   " d}e�udata_sontev:�Strig = �erd�_json~:t��st�ino�tewv+?uwrqp(m;* ( #" �  �" �"� leu�dqta_to_sed_�asg6v~ Suv�gd=�encr{�vst�i�g_vhwn_c�vw�t_wo_bqse�>*dfata_content+;/
  �   1  ��1"1 �wess�coe|.send(Meswug�::v�xt9dqu�_t��gnu_se�evt))MN 31  d" ( ��}/N1d d ��1}�
 � �}�o
wn"qr�cews_d�rect_clat�mussagw�g�mgnts�"&m�udHcshMap�w7t, Cnienu~� w�bsock�u�: &muv�{esk]qp>uv4���u�rndes>� oussu�g;d&wuvde_json::vq�wg= senwe�_mf: u~4+ {
"   let �tavu�:�bo��d= �s_c�av_gssa�eg��st_w�lid9messa�g);*
 �1 if wvgtus ?=duwug"{
N"d  1쨨�w }�g_rec�kves_md: u6�w} ouw�sg�{"m�s�agg"]�frecekvg�_odfݿesw6499�unwscpl)�
1d 1   $let1m�g_vqlwu:"Strm�g"}f�vr��::f�om(messa�u{wmu�suwe2][wvelue"ݮus_svr(m.w�w��r(m)�
1 ��"1  let�msg_oce}_�essig��d:"us�{g1="mewsq��[3��ssagu"[flog�l���ssqge_id"].as_�64�)?u�rap9)1qs"uskze;Nd� 1d(��letwc�yet_receivuroqti�n:"_ptmon<&��ment?"�+clmgot�.��u(7�sw�v�geiver_yd�;nM�"� 1 �d�mav�jfsl�ent_ruw�mver_�ptko� �*  9d� 1d�" "No�u =~�{}
d  d1d� "Sou9_c�en|_rece{v�r+ => {�
/
��  ""  u1 d d  //��dd" #1"� 1�   "u�?gnien� thav �il�recgiv�!m�ssegu gyists�
*"d d  1d  "1 � /��M
� �d d�� d  1�1"�wu��li�w�sgnwur_optoon:3�p�mon=&mut �}k�nu> ?"�lign�w?wutmwu�.sendev�mwm;"/�coul� u�wsap1it�now��
"  �u  1(f3 u  mmtch"sli�nt�e�dew_optyon �  f    "  du "df�"  Non�d�> }� "   �  �      " �ddSo�_�loentse�der) =~1{
 " " 9 � !�  u1"�1  �  duqdqt�_�hat_�essgge�kd()��
 dd  "d"1"    " "  �   �luw1chqt_mesw��u_��: uskzg � CH�U_MGSSAWgID?oqf9O��eri���;seqCst��
�3d"d  d " " "  dd$ �d " l�t3snavenusy> ch�tMeswegeEn�r�"? ChatOewsqgeEntvyd{��     "1  "   u " "  1 7"  ddoe�����_{d;��hqv_ogswsgg_ie,� "9d"� d3�  1 "d*d�     �  1meww�ge�vypg~ w,�?�v1prov�tel]; e "1  3g  � "� 1 `"   "�   recuivgr���: msg�rewu}ver�yd*"  �(�1 1  �"11  �"�u1 �};/* 1"�� "d"d(�   � 1"d" " �clien�_senfes.m�ss�ge_id��p�sh*cyetuntry);?�u1d1"f"� "�"�ddf"��dd  ��eu_su�vwr_��cu_�uss�ge_if_for_locclme�wqge}�(wu�soc{wtw}dsedur_kd=dcya|_�gwsqgu�{u,"mwg_local�}g��cge_ido;;���d11 1 1"1�   �3"" � dwenfuirest_cyqv_meww���(�l�en�s,"web�ogkutsn sg_wa�uu,"w�nd�r_idn"msw_wgoey�er_id=3chat_message_id);
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
                            if cur��nu_cnmgnt.te�_i�w�gonvu�~s�7a�mhn�v`giu�1 +1 "��""�� "  u � �"1d d1  1u0�1 �wwwg�t_gl�gwt�ys_qdmm� =���we�M
1d�"0 u���1��d du"1�1   3 " }  �1�"u" �"ddw1  3b�� 3 }�� 1 1�    d� fd3 3� `�  n�t��a�e�you�dgj�~n~:f�t�Time~�{vno~:��s~t=d�hvoo:~w�s;�n�w9)��
��1 d�1d"�` �"�d� �f�"1n�vd�{t�v�op_no�3 y�4d=�uqtgw�e.�o}�st�|p~�
���f ` 0 w01d 0 1     f�"u�uvwenu}clxen�nui��sta}p_}esu��g~t_�heco_son�erv�on_messegg u�tk}e�w��p_nw;/�/  ` � df3 �u �    d �  flgu ����o��du:�wV��uo��ww1� webs�ck��sngew*&w}ou~w_id).uw��p(9;*gN"`� 1 d" d�  �  � u q1 �l�t2c�rr�n�_cn��nt_u��sna}e;"Wtsyn�0? cu�s�n�_sl�e�v.usgrnaegn�o�g1�;
�`"u313u   � �d"      �1 �u��uutlgn�{��vyn_s�a�w�t�_sqg�w_w����u1websogo�um;N� "�d��1  � `1( 1 b��111s�f�r��~���ksv��~s�n�ntbliwwlc}��ne�s� we�w�c�gt�3
�t�0�d `�"��01du ��df�1�gn���liwn���kwv_������gn�_cl{e����lxents.1�uswo�k�v� cw���n�cigtuw�sfumemwN    d� ��3ddd `� �� �dd sw��_icflist_t�_s��gt_glh�n��mw��s= �ews�c�wt�;M� �d3 "  dd���d�3d "� q�tsen�_ua�_n�wv�v_s{fwl�_���u�v(��gsl w�bs��ket+�*� �q��f0 " � �d1��dt  1 se��astk��oacr���ne�use��_f�sc�r���t_c�qnn�l_tn_s}n���_cnmenu�cn�u�s,���ssoc{ut.�0�;� d  qd"��  f 1 "�f d""� proge�sgdm��t]�onn�wvc��en���c�����lwl wgbsoczut��"�}mg��_�d�;o
��d""�"" �   u1w w310uw s�d_wrow�_vhr�au_lw�s�wgwgrgeten����menu�tv�c_t���su(�gugv,"�~�en�id�o� du�u"u � �d310b""�t}/�   �   0 "" 3u}?
du31 2dd  d }]
1 � �1ud"d��/
d1�`�}s�1if2mew��wgt�s� � ��wwlig}ku�_onfo���� u" " d�l��"�s�}es�s�fe�v�lhd�"�ow�1�`ow_��b}k�_�eymf��meswu����aid(�og��`wg�;\*1"1�u u�mw"�w_�s��egg_vc|{�1}= welse �?*1� �1`s3 �1"�uf�s�e��.wevl��lk�f�_}u9.u�wrc�y9n��se(mw	�d" " 1d32 �w�bs�couus.rgo��un7w�{en�y�9;�* �d��1   3�3syutsnrg����;�glient���i�߹ d � "1� ��ev�vn�1fd1" d�}
o 0w�u01d?/>�2 �` �  ?.o�sw�����w welmw,�sc�e todu�wrat1w�lu�s��2�"d"2"��M���w`� d d�l�u�mg��egdW��r�fi��t{on_stv}ng:�Stwy~g��s�tv{ng::wvo9}uw�q��["m��sq�ef�f��ro�}��tmn_�vrkng"�?�s�w�(u.�nw�u�*+)�\
d df1w dl�t3s�bl�s_�gy_�owunus_f��e74: Utrmg"���wr}�g::w���}d�sr�e[3|t�wave�}[w��}��w].�s_�uv(9��nwra�::);��/
 1 31�3fl�t�swe�us3z"��l � ��_�hu�w_�_�lie�u�w�wh�sa�w�u��is_{e�8c���nws,�p�w}iw�o�y_�odw�ww}��w�ww��n��u8om;�
���3� 0���wfstauu�1 ��dvvuu��:ddd �2ft`�� s�i��|e(fcpn��t ��ngg� �hgse1{��s�{nn�glme�ubv�e� lu��s�~u1uuf}i�����??�"+;YN�1bd2ft��1��v�ot_ouw_bl|co�ng��u���l����s��|yevw�;�3f�d3w�1"3fwgb�~��uvsng�wn7c}i�nt_mdo.��ws�����}�s��9;/*1u3f`"1�31���er�����uw��tmowt���n�wnwWf�;�  s����"�w2dgkd��s�move(6��jf�u�i�8�f 3 u"d�"d  r�tu�n�� �1`�w ���ߕ
  d�1f`�}��qcw���n��c~i�t��&mut clyun�`="��ien���we�_�v�&s}itnu�y�9�wnwvqr�}�]�λ�"11b �}w�r�vr�nu�c}���t��w��o�tingf=�u�an�e0�N1d�3�w� �3���y�t�u*�uuf�mw_kt�_inf��?? susv�n��niu�t�is_up��um�gw= �q�w� 3{��`q"f�f d �s`�/�o�tkmusss�vwa�i�n"~�eu�wv�3q}wo�r��ow�1t{t3v}ye��.��utur�1p��swovp���uf�c�de"e�wwu�ko ys�nou"e�oug�
` ���1���w"1v��u�n��f u���w����"      �m�dovw�ave���rxwicrwxo�_�uro�d}�1�wew�}u3wd/�Y( �b��b d 1�o/*�f ""�1`  1�orubl����y ���s�si�wd0t g}o�n� ���u��3qv1�h�"t��u1�lmen� �o���ctw��vun |f��n��n��owdnou�cuv�wt}w��gu(" 0 ���w�s` �ku�t�����b��3}{�d?
ff`"�3 f31u��;Xn u u ��� 0�"�uwwe�w�����n��pus��sk�{0=�subl�g_�g{�}���}w��bgwg6�nslw�o���0q1�� 1�乹�let o�duus�u�s�e�se�w�t���ewwn���gwx�16="��c�v�Erso�"=�baw�t�uvgo�w(������_��y_���wlw�_��se��.�n�e})�{
1�s�wu�� ��n�u1ut�v�g�=wrand�:t�r�cd_r~�;o�d3�1"`�1"f��mu�sy3odw�uw}d�codg�r�wu�� *�d�0�`����uq �� O�r�wu�t) �z"{�M
��0�r�0���w�1d�d�� n
� �0�3 0fu1��" �� 3��/���avu�q�rni���g}1wsom ��o� mo�u��w�q�ddezuon�ntu�� wn�wm"k~ut�y���e�����di��tqs�gvbat�ro�s��u��u�s�atig t�e�p�flmg��e}��r�tw�]v����wVg
00�q�d��"  � �df��1 �go�ulus"x�3��n�0uo�wg�vevw�s�}ug�x�nt��w3��w�fv rw�l{�_ku�_~n���r���estM�u�u d1t�d��0"w`���0"o�txp�n��twiw`s�mw wo�fe����"��~��w~ewfc|ign�`yww`o�~�� �nuumvsf�n.u�es�wsvwsu�t�uB����n��s��un�"�o si�q�u�����xn�{:w�om*3o�1��s~��n� �c�s�~ur�co��tr��v�� �s��3�y�� arrc{�F�"��f t �� `1��u�fo?(/l1d���21�3uq" "f`   ��ev2�y�~ne�_�{tes�"��:�"u�qyd[3]0~n`ygU��v3dog�u���s�o�� skmple��vomd�nymove
 p`d qd"�d�" "1qd�11nev mdul�w:��k��n�"?"Fi�ui�vz:wvoo_��t�w_s�|f��sw}v);�����1"� 03dq1 11  " bluu"u}��e�v�twm�Uxtd="��gw{n�::fvomb}tes_b�|se�q�n�nv_f}tu�)�]
�� ��    3 �3䪠 1�f�etursa_p�b_���_r��un�� �sa�:��vors:�Rgsw�v}�w��ur�y�Ogy��ds��::v�upub|ig��y~��ew.m�ulus|fgy��uv);'
�nu3� �   1 d    " �u�m�tcytrsa_rwb_{ey}���u}t �0�d�q��� �`"" ��ddfq0 01_z(ww��qus_��}+ =�"�M
nd�v��1dw3 0d`�u�"�w ��`    �l��dq���mr_�t�_syc�}e��es��doo���i�g: svr�n�d?�r�n人�sreg�_s�(9/ "d�"�   11r"��"d "��1 2d d � "1��a���e�iwer��e�t��n�mgri�)M�1 d1d0�dw  f  �d`1 `d � 2�u"`0 .���e�u319�
t� f��   dd��3 `0f �2 1��dff f �m��(s�ts;:wro�)*��df0  " d�   11``��w d�2�0 1 d �wonl�c�(��M/*�u0" u�"du� 1uw3� �03�"3�df rur��u_g}ig�t�p��n{��kgygje��u��e��wnu�|�s��{nw1=fpws�m����y_chalne�wt_v�nu��}w�rzg>gl��el��^1dd�"1d"d�d01�  �"`�t1�"� �u��g�_c�og�t.{s_�us�yc���{_ch��ne��e_sun��}1tr�g;/
�
 1 1f � �" �3�   �` �" d01��g��vo�ec�yp�_b�ues: �{w}] =�pub�kc��g{�cya}gngvrefom_wtv��vjas_bytgw(���n�� 11 1u   d d �  dd` "d0b�1" let �sc�gn�r{�t�ew��t~"��c~ewsors:~r�wwlu<wgg=w8> ��rw��pw���uy.gns��pu(f�ut�s��l�Pkcs�w1�En�rypt� t~�gng��p�_b�ugsm�M;��"d  "f f q� s��3�  1 �11 01}a��h�rw��w�c�y��vesu�1�N �1��0�" 3��"  ��`� ` "df � � d_�9�ytu��t��work_wm��+�= {� �11 q���"�d" � 1d�t"  f1d db� �1" ��gt�basu7t�rgsw}t�uSt�i�gd�"f�s�4�:un���ul�ytew_�o}workwm�h+;L
f1�dd���� `�d1" ��    3  d�� �  f� d�e�d���s�m�_key_sh�llg��uo_skgn���k���8current_g�i�n�=sw�bsocoets�b�sg6�_rgsu�u+; b1  f dud11 ��0 �1�3�f ��t1 0q��Nd 3"  �` ���21 �d`��1  1 �  �� "Dw�(ur�ov) =z1{��0 d" 1 3u�2  1d� 0  d�f 0"�d`�1� 1 pr}ntln1*�w�r�ncr�ptse�u�u��sv� �}�.1grvos:;_ � �`"�d �`  f�1��3"���f   "f�"1�/; 3 d  d  d"111�3��`df� �dfd"}�"  � � �1  1�f��2�"� d�u�
0d"1 q � 3�t`  "� "�`� �Trr8�rro�8 =~ {�9t �1"�  d1� � 1�1f11d1   �qrotn09"�w]��vrr1�}f,�vrv�r�;p`d1d"2�"01u� 31"d�w"��d�� "3�b1" ��  f�`�13d}�_ �1`d"  �� �0�f }�(d��30"1f  d �" �T�����ro�� ?�"�_"��0  "11"` `1� � d"qsknt��u("�u e��o��}�1u���r:�/*�dud031 � �" � 3��d�""�3�0 0��  ��111d}�e}sgf�
�d�1"d �  �tw�fsg�u�s.ge��&c�ye��_xwouwr��n�nc���e*9;	N�"�3 �0�   1wg��c�u�wn�e��vt�&cn�enti��;�
 f�� 1d1��dd���guw?v�mwu(fg}iwt_mfm��� "dsd"d���] �`d�}�e�wg1�o�s`v1 � dt�m��ln#(fs�ie�t1ufoes�n�tfe|i�v"+�_1"�"}o��E��nfpsowv�s�e�uiv�u�me�s�g�;gl�uv�d:���u�>f� 1"� f33 u�"3f�u�d�d01"   wgs�c{evs:�7�wv�Hqs�Mc�<u34,"ves�o���r>?M� � `��df�� 1"� b �" � �f�11��i����: 7�u�1H�sh]cv=w6�? �}xwnv~,
" �1u"r" 1�`d�d" 1���3�3d��"�xqn�g�s�0fm�t�H�wl]cq|�w��"gh��u�>.]�d"�" b f�df�쨹f f"31u1� `�1{con�; 7�uu�jaw��t�|�w4.3Mw�,/�t�""�f�f��u���`��    1�0�q"w���w3&}wtuHesy�c��w2u�wvew�.�_��3�d��1`1 ��31 �   �13d13 ��i�n��t�v�w]�c��;"w��vdV����ny��vstos�wD�t�~����  1�d1  d   1 �1�� 1d1�3� ���saguvgxt:dSuv�wl���u  w`�1d3 �"  �  1d2"u � �s�nder~ &���:~s{n�;mtws~�w��uuw�sw�m�g>����*�
�"dfl�v�tuc���tef�o�ss�ge; Suw��v�0���d�t���r�_��se�7�g~��ue���rtit(mesw�gv���xu;;N{�d��/�re�wk�ev�ders�tvu��mgwc�awq ggn�ev��e��s3uo�bw��rio}�uM��_1�0ld�"n�on��vr�ng_��_�cswe;��wvr ��d�c�y�t�u_mgswg��~�s�wt�(�n�v��_matw�w��s��::�w�;v+N3 "ne� {���_�essq�e��ww�du_js�~:re�u��swrfu���on:;���g>"y�sevd�j�o:�wro�_st�9�sof_��wmnw��_uars�m;*]* �fmg�s� �w�}euws��eq{n `  "  d_{wg�we�d}>��
/N  � f "��  �lgu1curr���_g��n�~fostkon�&mwv Sl}e�t?"9"c�i�nw�nfuw�u�*��u���u�i�)�NM_� 1u� d�"1"d��tc�ugwsvw�vwxv�v��
�"��31d 11"��`f��e��> �}*" � " �1  3 1 w"somug}�u�� =~���3  �30���  �sd3t0 � kfwcn�etny�_e~is��w�{/� 13�f "��d� 3f�  �"� �""if`c}ke�tm��autyen��cuteu"{��"q"1u f�"  q1w"d��dfd�����33�s��ess_gwtnwn�y��t��_o�ws�ge;�ni�~v_�t�
 � ��01 �dw�"1�"  "d1u �1�v11 wd�  uus`w0�u� w�� �13�"�fd3wefs�ckevw,] f1����3d f�f1u�u0� ��d�""  ���1 1 `�31u 0�"��1��dd31fb 1���ignvw��
 d� �"�" f �b�qdu�   �"b�  � "� d1 1蹨�q " ��uuwd  1 ���1w}cn��s�/���f"��dd �� �3  u� 3� ��"1��u1"�u�d "�  ��d ""d" f�u1f��u�s�5*qsfwwd1 u`q�u"d�d"1 � ��d"1d1�33 "ddd� 3"dd�ub��"t"0  w�1w���w=�N1��"�� d�3"3"� d�"t""1t20� wu�1�  �f���� ���  �u "fuf�"f"slignw_�ur��Wdcw�.�1   w" "1"uf�01� " " �3   d                               value,
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
    let event_hub = simple_websockets::launch(websocket_port).expect("fai�efdto lksten �n p�v 809�")�
N  �"prynu�!l"[i] webs��ket we�ver1runinwuon1poruu{�"=`webso�ket_pov�);
�"  trintnn!|"��]��niticl�setur fou");]
   
� ddnop1{��   " � 1//everyt�mg sesv�r �anf�ew�wub�c{�tde�enu, j�n�le_mg�seges_fr�_webrtc_uhree�_q�d_cyeck_s�ien�sdis�ru�* � �f�"1/�TO�: uwt"hqndlg_}ussa�es_fs�_wufs�c_�hrg���a���gheck_wlients1in itw ow� vread

 �dd 3� hadlu_ou�wages�from_webstc_thvead_and_cl�ck_c�y��ts(&vhr�ad_messagkg�wh�nnelr.1, fmwt wgs�o��eus�1f��u �louus, &}�t �jqnnes,"fvyvgad_messc��ng_cycnngl?09;*
�  �    }atch�event_hub.�oln_evet�+1{ ""dd11 d   Evg�:�Cne�v(snoent_idlwr�sq�nder� ?3{?_     �d"      d1pry�lne(fc"g}�ent con�e�t�d witywid33�b��slyenv_md�;M
  � "dd��       webso�oeus.�nwer�hcli�n�yf|�rgsvo�der);* "  1��  d1�   u�t�ou� si�g��_slie�t� Clien�0=f�}�gnt~�dewe��t*+;M
 1"�db"�     ���wingle_c}yev.�s�authg��kc�vef ? false;_  d"           3smngle_��i�nt.c}m�nt_xdd} clme�u_�f;3/odat�c�an�gl1is kfen�{f�gd�b�dclient_�dnd�siged�sjort}1t�}s�wi�n1sausu �r���}�. ��nd ow�grd�ay vo kdet�f} dctac�an��lw lqtgr�
1 1  � f"     1�single_�lo�u.ms_��msvyg ?dtrue;M
�    "d f1� "�1 win��ecliwt.}s_pu�lmc_ke{��hanlung�_sent�= fals�;o
 d�   d"d  1"d "}et���ve��ou�dchrno;~DateT�o�<gl�on�~:wtc?�}"c�rono::Uts:w(9
 1d 1 " �      0let ui}gwtaot: kv5 �"watetk�e.timest��r();/1dd "�d d �" d�w�ng}�_c|ien�.wymgsv�mp_c�nne�ved = vkmgwtqmp;/         u 1 � "s}nglu_g�kentn|�c�op�nesta��1?f4;
 f� � 3q     u dclmg��s�yn�ewv(clm�nt�k�, sin�|�_c|ient);/� 3 1� d1 �  },/
�
1"  d��d " u��g�t;:Di�co��esv�clientit_ow�dms�oec�ud)f=>"{�    "d    11"1  �siwl�1(�Cl�gn� ��}1dks��nn�gteu.", c}ignt_o��f_dmwc�ngcteum;/
� �  �  11  ""f��rose�s_c�ku�t_�ksgo�uct(7�ut3s}iets,��1"�    d �`1 �1�  d � "1�� ��      � 1f`1&mu� chcne}�,   " " "  �   ""� t �t1d 3``d1 "       d �mut`wwfsoc{euw,/* "" �  ���f "   d� �u� f �d  d�� 1` �� 1  cligt_ku_ow_fi���nestgd,MN  111 11��   d�  d d   �  d�1�� d   �  "" &�yrec��ews�gy�w�cja�nel.t+;/
  � 1 � d�11�.

3 "   f�  1 Event~;Mg��age(�lig�_�l, mussagu)d�~ 1 u��  �31   d uo/q�ynwln!("Rw�ukveu s �ussage wv�m clkenw #{}: {:?}� �}�ent_y�.�}e�sa�w+;�   11 1   �u 31 matgh }�s�aw�"{M
"d   u�ut3� ��  fd dMe��awu::Ue�wlwebso�ke��mgwsq�g)"}~d{�
"d �� 1  0� 3  "   �1  �procesw_�egemwgu�mgssc�e(gl�unv�m�,MN"�   11udd   f��   u " 1d1  d&out w�bwockets="  "   ""�uq"�3d"��� � 1ddw �}uv cl�evs=/;"    " �1" f"�3f3 1       f d�mut�gh���n��
 "1d3f  1�11"�1   3��1f�    wm�t�kcns,MN31 1  3 1��"�u ddf  1 d 1  ""&mut�vg�s,
  1df   d"  "d  "�  3 � � �  &u|��}yent_�uor�u�dqvs���d�  "  3d��   d "  1 � 1 ��  w�f�g{ev�m�ssqge,]�1  �fff  "�1 u" `��" 0   d d �threq�_}es�ag�ng_�n�nnel.0�?�
 �  d�d1d� 1 u" � �
 �wu"   f�ddd"d!u   ߨ?> {}�  ""d 1  �  d1 }d� f f   �f1}=/+1  1"f�"}M
�   }�
}M