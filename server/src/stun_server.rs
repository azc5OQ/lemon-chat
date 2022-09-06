use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use rustun::server::{BindingHandler, UdpServer};


pub fn start() {

    let bind_address: SocketAddr = SocketAddr::new(IpAddr::V4(Ipv4Addr::new(127, 0, 0, 1)), 3478);

    let server = fibers_global::execute(UdpServer::start(
        fibers_global::handle(),
        bind_address,
        BindingHandler
    ));

    match server {
        Ok(server) => {
            fibers_global::execute(server).expect("stun handler panic");
        }
        Err(_) => {
            println!("stun handler panic")
        }
    }
}
