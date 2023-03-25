use std::collections::HashMap;
use std::ops::{Deref, DerefMut};
use std::sync::{LockResult, Arc, RwLock, RwLockWriteGuard, RwLockReadGuard};
use std::sync::mpsc::{Receiver, SyncSender};
use std::time::Duration;
use tokio::time;

use webrtc::api::APIBuilder;
use webrtc::data_channel::data_channel_message::DataChannelMessage;
use webrtc::data_channel::RTCDataChannel;
use webrtc::ice_transport::ice_server::RTCIceServer;
use webrtc::peer_connection::configuration::RTCConfiguration;
use webrtc::peer_connection::peer_connection_state::RTCPeerConnectionState;
use webrtc::peer_connection::sdp::session_description::RTCSessionDescription;
use webrtc::data_channel::data_channel_init::RTCDataChannelInit;
use webrtc::ice_transport::ice_candidate::{RTCIceCandidateInit};
use webrtc::ice_transport::ice_connection_state::RTCIceConnectionState;
use webrtc::peer_connection::offer_answer_options::RTCOfferOptions;
use webrtc::peer_connection::RTCPeerConnection;
use webrtc::peer_connection::sdp::sdp_type::RTCSdpType;

use lazy_static::lazy_static;
use serde_json::Value;
use webrtc::data_channel::data_channel_state::RTCDataChannelState;


struct ClientLite {
    client_id: u64,
    channel_id: u64
}

lazy_static! {
   static ref CLIENTS_LITE: RwLock<HashMap<u64, ClientLite>> = RwLock::new(HashMap::new());
   static ref DATA_CHANNELS: RwLock<HashMap<u64, Arc<RTCDataChannel>>> = RwLock::new(HashMap::new());
}


#[tokio::main]
pub async fn webrtc_thread(

    thread_message_channel_receiver: &Receiver<String>,
    thread_message_channel_sender: Arc<SyncSender<String>>) {
    
    let mut peer_connections: HashMap<u64, RTCPeerConnection> = HashMap::new();
    //  let mut rtc_datachannels: HashMap<u64, Arc<RTCDataChannel>> = HashMap::new();
   // let mut clients: HashMap<u64, ClientLite> = HashMap::new();

    loop {

        for thread_channel_message in thread_message_channel_receiver.try_iter() {
            println!("[i] webrtc_thread received message from websocket thread");

            handle_received_thread_message(
                thread_channel_message,
                &mut peer_connections,
                thread_message_channel_sender.clone()
            )
                .await.expect("TODO: panic message");
        }
        time::sleep(Duration::from_millis(1)).await;
    }
}

async fn handle_received_thread_message(
    thread_channel_message: String,
    peer_connections: &mut HashMap<u64, RTCPeerConnection>,
    thread_message_channel_sender: Arc<SyncSender<String>>) -> anyhow::Result<()> {

    //println!("received {}", thread_channel_message);

    let json_string_to_parse: &str = thread_channel_message.as_str().trim_matches(char::from(0));
    let json_message: serde_json::Result<serde_json::Value> = serde_json::from_str(json_string_to_parse);

    match json_message {
        Ok(json_message) => {
            let message_type = &json_message["type"];

            if message_type == "client_connect" {
                println!("create_new_peer");
                let client_id: u64 = json_message["client_id"].as_u64().unwrap();
                create_new_peer(thread_message_channel_sender, peer_connections, client_id).await?;
            } else if message_type == "sdp_answer" {
                println!("set_sdp_answer");
                set_sdp_answer(peer_connections, json_message).await?;
            } else if message_type == "ice_candidate_from_client" {
                println!("set_ice_candidate_from_client");
               set_ice_candidate_from_client(peer_connections, json_message).await?;
            } else if message_type == "channel_join" {

                let client_id: u64 = json_message["client_id"].as_u64().unwrap();

                let clients_write_lock_result: &mut LockResult<RwLockWriteGuard<HashMap<u64, ClientLite>>> = &mut CLIENTS_LITE.write();

                match clients_write_lock_result {
                    Ok(clients_writeguard) => {
                        let client_option = clients_writeguard.get_mut(&client_id);
                        match client_option {
                            None => {}
                            Some(client) => {
                                let channel_id: u64 = json_message["channel_id"].as_u64().unwrap();
                                client.channel_id = channel_id;
                            }
                        }
                    }
                    Err(error) => {
                        println!("error {}", &error);
                    }
                }

            } else if message_type == "client_disconnect" {

                let client_id: u64 = json_message["client_id"].as_u64().unwrap();

                let datachannels_write_lock_result: &mut LockResult<RwLockWriteGuard<HashMap<u64, Arc<RTCDataChannel>>>> = &mut DATA_CHANNELS.write();
                match datachannels_write_lock_result {
                    Ok(datachannels_writeguard) => {
                        let datachannels: &mut HashMap<u64, Arc<RTCDataChannel>> = datachannels_writeguard.deref_mut();

                        let result: Option<Arc<RTCDataChannel>> = datachannels.remove(&client_id);

                        match result {
                            None => {
                                println!("nothing to remove from DATA_CHANNELS");
                            }
                            Some(_) => {
                                println!("entry removed from DATA_CHANNELS");
                            }
                        }
                    }
                    Err(error) => {
                        println!("error {}", &error);
                    }
                }

                let clients_write_lock_result: &mut LockResult<RwLockWriteGuard<HashMap<u64, ClientLite>>> = &mut CLIENTS_LITE.write();
                match clients_write_lock_result {
                    Ok(clients_writeguard) => {
                        let clients: &mut HashMap<u64, ClientLite> = clients_writeguard.deref_mut();
                        let remove_result = clients.remove(&client_id);
                        match remove_result {
                            None => {
                                println!("nothing to remove from CLIENTS_LITE");
                            }
                            Some(_) => {
                                println!("entry removed from CLIENTS_LITE");
                            }
                        }
                    }
                    Err(error) => {
                        println!("error {}", &error);
                    }
                }


                let remove_option: Option<RTCPeerConnection> = peer_connections.remove(&client_id);
                match remove_option {
                    None => {}
                    Some(_value) => {
                        println!("RTCPeerConnection purged from peer_connections")
                    }
                }
            }
        }
        Err(error) => {
            println!("error {}", &error);
        }
    }

    Ok(())
}

async fn send_cross_thread_message_sdp_offer(
    thread_message_channel_sender: Arc<SyncSender<String>>,
    client_id: u64, offer_string: String) -> anyhow::Result<()>{

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

    json_root_object.insert(String::from("type"), serde_json::Value::from("sdp_offer_from_webrtc_thread"));
    json_root_object.insert(String::from("client_id"), serde_json::Value::from(client_id));
    json_root_object.insert(String::from("value"), serde_json::Value::from(offer_string));

    let data_content: String = serde_json::to_string(&json_root_object).unwrap();

    thread_message_channel_sender.send(data_content).expect("send_cross_thread_message_sdp_offer except");

    Ok(())
}

async fn send_cross_thread_message_on_peer_connection_state_change(
    thread_message_channel_sender: Arc<SyncSender<String>>,
    client_id: u64,
    peer_connection_state: u64) -> anyhow::Result<()>{

    let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
    json_root_object.insert(String::from("type"), serde_json::Value::from("peer_connection_state_change_from_webrtc_thread"));
    json_root_object.insert(String::from("client_id"), serde_json::Value::from(client_id));
    json_root_object.insert(String::from("value"), serde_json::Value::from(peer_connection_state));

    let data_content: String = serde_json::to_string(&json_root_object).unwrap();

    thread_message_channel_sender.send(data_content).expect("send_cross_thread_message_on_peer_connection_state_change except");

    Ok(())
}

async fn send_cross_thread_message_on_ice_candidate(
     thread_message_channel_sender: Arc<SyncSender<String>>,
     client_id: u64,
     ice_candidate: serde_json::Value) -> anyhow::Result<()>{

     let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();
     json_root_object.insert(String::from("type"), serde_json::Value::from("ice_candidate_from_webrtc_thread"));
     json_root_object.insert(String::from("client_id"), serde_json::Value::from(client_id));
     json_root_object.insert(String::from("value"), ice_candidate);

     let data_content: String = serde_json::to_string(&json_root_object).unwrap();
     thread_message_channel_sender.send(data_content).expect("send_cross_thread_message_on_ice_candidate except");

     Ok(())
}

async fn set_sdp_answer(
    peer_connections: &HashMap<u64, RTCPeerConnection>,
    message: serde_json::Value) -> anyhow::Result<()> {

    let client_id: u64 = message["client_id"].as_u64().unwrap();

    let connection = peer_connections.get(&client_id).unwrap();
    let mut answer: RTCSessionDescription = Default::default();

    answer.sdp_type = RTCSdpType::Answer;

    answer.sdp = String::from(message["value"]["sdp"].as_str().unwrap());

    let set_remote_description_answer_result: webrtc::error::Result<()> = connection.set_remote_description(answer).await;

    match set_remote_description_answer_result {
        Ok(_) => {
            println!("sdp answer set");

        }
        Err(error) => {
            println!("isdp answer set not {:?}", error);
        }
    }

    Ok(())
}

async fn set_ice_candidate_from_client(
    peer_connections: &mut HashMap<u64, RTCPeerConnection>,
    message: serde_json::Value) -> anyhow::Result<()> {

    let extracted_candidate = String::from(message["value"]["candidate"].as_str().unwrap());
    //println!("extracted_candidate -> {}", extracted_candidate);

    let extracted_sdp_mid: Option<String> = Option::from(String::from(message["value"]["sdpMid"].as_str().unwrap()));
    //println!("extracted_sdpMid -> {}", extracted_sdp_mid);

    let extracted_sdp_mline_index: Option<u16> = Option::from(message["value"]["sdpMLineIndex"].as_u64().unwrap() as u16);

    //  println!("extracted_sdpMLineIndex -> {}", extracted_sdp_mline_index);

    let client_id: u64 = message["client_id"].as_u64().unwrap();

    let peer_connection: &mut RTCPeerConnection = peer_connections.get_mut(&client_id).unwrap();

    let mut ice_candidate_init: RTCIceCandidateInit = RTCIceCandidateInit::default();

    ice_candidate_init.candidate = extracted_candidate;
    ice_candidate_init.sdp_mid = extracted_sdp_mid;
    ice_candidate_init.sdp_mline_index = extracted_sdp_mline_index;

    let add_ice_candidate_result: webrtc::error::Result<()> = peer_connection.add_ice_candidate(ice_candidate_init).await;

    match add_ice_candidate_result {
        Ok(_) => {
            println!("iice candidate set");
        }
        Err(error) => {
            println!("ice candidate not set {:?}", error);
        }
    }

    Ok(())
}

async fn create_new_peer(
    thread_message_channel_sender: Arc<SyncSender<String>>,
    peer_connections: &mut HashMap<u64, RTCPeerConnection>,
    client_id: u64) -> anyhow::Result<()> {

    let thread_message_channel_sender1: Arc<SyncSender<String>> = thread_message_channel_sender.clone();
    let thread_message_channel_sender2: Arc<SyncSender<String>> = thread_message_channel_sender.clone();

    let clients_write_lock_result: &mut LockResult<RwLockWriteGuard<HashMap<u64, ClientLite>>> = &mut CLIENTS_LITE.write();

    match clients_write_lock_result {
       Ok(clients_writeguard) => {
           let clients: &mut HashMap<u64, ClientLite> = clients_writeguard.deref_mut();

           let clientlite: ClientLite = ClientLite {
               client_id: client_id.clone(), channel_id: 0
           };

           clients.insert(client_id.clone(), clientlite);
       }
       Err(error) => {
           println!("error {}", &error);
       }
   }


   println!("listen_for_connection");

   let api = APIBuilder::new().build();

   // Prepare the configuration
   let config = RTCConfiguration {
       ice_servers: vec![RTCIceServer {
           urls: vec!["stun:127.0.0.1:3478".to_owned()],
           ..Default::default()
       }],
       ..Default::default()
   };

   let connection: RTCPeerConnection = api.new_peer_connection(config).await?;
   peer_connections.insert(client_id, connection);

   let peer_connection: &RTCPeerConnection = peer_connections.get(&client_id).unwrap();
    
   let datachannel_settings: RTCDataChannelInit = RTCDataChannelInit {
       ordered: Option::from(false),
       max_packet_life_time: None,
       max_retransmits: Option::from(0),
       protocol: None,
       negotiated: None,
   };

   let data_channel_options: Option<RTCDataChannelInit> = Option::from(datachannel_settings);

   let data_channel: Arc<RTCDataChannel> = peer_connection.create_data_channel("data", data_channel_options).await?;


    let datachannels_write_lock_result: &mut LockResult<RwLockWriteGuard<HashMap<u64, Arc<RTCDataChannel>>>> = &mut DATA_CHANNELS.write();
    match datachannels_write_lock_result {
        Ok(datachannels_writeguard) => {
             let datachannels: &mut HashMap<u64, Arc<RTCDataChannel>> = datachannels_writeguard.deref_mut();
             datachannels.insert(client_id.clone(), data_channel.clone());
        }
        Err(error) => {
            println!("error {}", &error);
        }
    }

    let client_id2: u64 = client_id.clone();

    peer_connection.on_peer_connection_state_change(Box::new(move |s: RTCPeerConnectionState| {
        let client_id_for_closure = client_id2.clone();
        let sender_for_closure: Arc<SyncSender<String>> = thread_message_channel_sender2.clone();

           Box::pin(async move {
               let peer_connection_state: u64 = s as u64;

               println!("Peer Connection State has changed: {}", s);
               println!("Peer Connection State has changed for client: {}", client_id_for_closure);

               send_cross_thread_message_on_peer_connection_state_change(sender_for_closure, client_id_for_closure, peer_connection_state).await.expect("TODO: panic message");

           })
       }));

   peer_connection.on_ice_connection_state_change(Box::new(
       |connection_state: RTCIceConnectionState| {
           println!("ICE Connection State has changed: {}", connection_state);
           Box::pin(async {})
       },
   ));

   peer_connection.on_ice_candidate(Box::new(move |ice_candidate_option| {
       let client_id_for_closure = client_id.clone();
       let sender_for_closure: Arc<SyncSender<String>> = thread_message_channel_sender.clone();
       //Sender would complain about "future cannot be sent between threads safely"
       //SyncSender solves this problem
        Box::pin(async move {
           match ice_candidate_option {
               Some(ice_candidate) => {
                   //println!("on_ice_candidate {:?}", ice_candidate);

                   let candidate= ice_candidate.to_json();

                   match candidate {
                       Ok(candidate) => {
                           let mut json_root_object: serde_json::Map<String, serde_json::Value> = serde_json::Map::new();

                           json_root_object.insert(String::from("candidate"), serde_json::Value::from(candidate.candidate));
                           json_root_object.insert(String::from("sdpMid"), serde_json::Value::from(candidate.sdp_mid));
                           json_root_object.insert(String::from("sdpMLineIndex"), serde_json::Value::from(candidate.sdp_mline_index));

                           let ice_candidate_value: Value = serde_json::Value::from(json_root_object);

                           send_cross_thread_message_on_ice_candidate(
                               sender_for_closure,
                               client_id_for_closure,
                               ice_candidate_value)
                               .await
                               .expect("send_cross_thread_message_on_ice_candidate except");
                       }
                       Err(_) => {
                           println!("error");
                       }
                   }
               }
               None => {}
           }
       })
   }));

   data_channel.on_message(Box::new(move |msg: DataChannelMessage| {
       Box::pin(async move {
           let clients_read_lock_result: LockResult<RwLockReadGuard<HashMap<u64, ClientLite>>> = CLIENTS_LITE.read();
           match clients_read_lock_result {
               Ok(clients_mutexguard) => {
                   let clients: &HashMap<u64, ClientLite> = clients_mutexguard.deref();

                   //current_client_id = client who sent message
                   //current_channel_id = id of channel where client currently is

                   let current_client_id: u64 = client_id.clone(); //is this only way to identify it
                   let current_channel_id = clients.get(&client_id).unwrap().channel_id.clone();

                   for (_key, client) in clients {

                       //yes, client_id is set as datachannelid..

                       if client.channel_id != current_channel_id {
                           continue;
                       }

                       if client.client_id == current_client_id {
                           continue;
                       }

                       let datachannels_read_lock_result: LockResult<RwLockReadGuard<HashMap<u64, Arc<RTCDataChannel>>>> = DATA_CHANNELS.read();
                       match datachannels_read_lock_result {
                           Ok(datachannels_readguard) => {
                               let datachannels: &HashMap<u64, Arc<RTCDataChannel>> = datachannels_readguard.deref();
                               let single_channel_option: Option<&Arc<RTCDataChannel>> = datachannels.get(&client.client_id);
                               match single_channel_option {
                                   None => {
                                       //println!("no channel");
                                   }

                                   Some(single_channel) => {
                                      if single_channel.ready_state() == RTCDataChannelState::Open {
                                          let future = single_channel.send(&msg.data);
                                          futures::executor::block_on(future).expect(" futures::executor::block_on");
                                      }
                                   }
                               }
                           }
                           Err(error) => {
                               println!("error {}", &error);
                           }
                       }
                   }
               }
               Err(error) => {
                   println!("error {}", &error);
               }
           }
       })
   }));

   // Create an offer to send to the browser

   let offer_options: RTCOfferOptions = RTCOfferOptions {
       voice_activity_detection: false,
       ice_restart: false
   };


   let offer: RTCSessionDescription = peer_connection.create_offer(Option::from(offer_options)).await?;

   let offer_string: serde_json::Result<String> = serde_json::to_string(&offer);

   match offer_string {
       Ok(value) => {
           //println!("offer_string {}" , value);

           println!("peer_connection.create_offer done");

           send_cross_thread_message_sdp_offer(thread_message_channel_sender1, client_id.clone(), value).await?;

           let mut gather_complete = peer_connection.gathering_complete_promise().await;

           println!("gather_complete done");

           // Sets the LocalDescription, and starts our UDP listeners
           peer_connection.set_local_description(offer).await?;

           println!("set_local_description done");

           let _ = gather_complete.recv().await;
       }
       Err(_) => {}
   }

    Ok(())
}
