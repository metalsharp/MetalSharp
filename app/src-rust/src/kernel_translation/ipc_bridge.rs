use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::Mutex;
use std::thread;

const MS_IPC_MAGIC: u32 = 0x4D534B54;
const MS_IPC_VERSION: u16 = 1;
const IPC_HEADER_SIZE: usize = 16;
const MAX_CONCURRENT_CLIENTS: usize = 16;

const OP_NT_OPEN_PROCESS: u16 = 0x0001;
const OP_NT_OPEN_THREAD: u16 = 0x0002;
const OP_NT_QUERY_SYSTEM_INFO: u16 = 0x0003;
const OP_NT_QUERY_INFORMATION_PROC: u16 = 0x0004;
const OP_NT_QUERY_OBJECT: u16 = 0x0005;
const OP_NT_SET_INFORMATION_THREAD: u16 = 0x0006;
const OP_NT_CLOSE: u16 = 0x0007;
const OP_NT_QUERY_VIRTUAL_MEMORY: u16 = 0x0009;
const OP_NT_DEVICE_IO_CONTROL: u16 = 0x000D;

const STATUS_SUCCESS: i32 = 0;
const STATUS_NOT_IMPLEMENTED: i32 = 0xC0000002_u32 as i32;
const STATUS_INVALID_PARAMETER: i32 = 0xC000000D_u32 as i32;
const STATUS_INVALID_HANDLE: i32 = 0xC0000008_u32 as i32;
const STATUS_ACCESS_DENIED: i32 = 0xC0000022_u32 as i32;
const STATUS_INFO_LENGTH_MISMATCH: i32 = 0xC0000004_u32 as i32;

static IPC_LISTENER_RUNNING: AtomicBool = AtomicBool::new(false);
static IPC_ACTIVE_CLIENTS: AtomicU32 = AtomicU32::new(0);
static NEXT_REQUEST_ID: AtomicU32 = AtomicU32::new(1);
static NEXT_VIRTUAL_HANDLE: AtomicU64 = AtomicU64::new(0x00000100);

static IPC_LISTENER: std::sync::LazyLock<Mutex<Option<TcpListener>>> = std::sync::LazyLock::new(|| Mutex::new(None));

static VIRTUAL_HANDLES: std::sync::LazyLock<std::sync::Mutex<BTreeMap<u64, VirtualHandleEntry>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(BTreeMap::new()));

#[derive(Debug, Clone)]
struct VirtualHandleEntry {
    handle: u64,
    pid: u32,
    tid: u32,
    access_mask: u32,
    handle_type: String,
}

fn lock_handles() -> std::sync::MutexGuard<'static, BTreeMap<u64, VirtualHandleEntry>> {
    match VIRTUAL_HANDLES.lock() {
        Ok(g) => g,
        Err(e) => e.into_inner(),
    }
}

fn alloc_handle(pid: u32, tid: u32, access_mask: u32, handle_type: &str) -> u64 {
    let handle = NEXT_VIRTUAL_HANDLE.fetch_add(4, Ordering::Relaxed);
    let entry = VirtualHandleEntry { handle, pid, tid, access_mask, handle_type: handle_type.to_string() };
    lock_handles().insert(handle, entry);
    handle
}

fn read_exact_u8(stream: &mut TcpStream, len: usize) -> std::io::Result<Vec<u8>> {
    let mut buf = vec![0u8; len];
    stream.read_exact(&mut buf)?;
    Ok(buf)
}

fn pack_i32(v: i32) -> [u8; 4] {
    v.to_le_bytes()
}

fn pack_u32(v: u32) -> [u8; 4] {
    v.to_le_bytes()
}

fn pack_u64(v: u64) -> [u8; 8] {
    v.to_le_bytes()
}

fn unpack_u32(buf: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(buf[offset..offset + 4].try_into().unwrap_or([0; 4]))
}

fn unpack_u64(buf: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(buf[offset..offset + 8].try_into().unwrap_or([0; 8]))
}

fn ipc_ok(return_length: u32, data: &[u8]) -> Vec<u8> {
    let mut resp = Vec::with_capacity(8 + data.len());
    resp.extend_from_slice(&STATUS_SUCCESS.to_le_bytes());
    resp.extend_from_slice(&return_length.to_le_bytes());
    resp.extend_from_slice(data);
    resp
}

fn ipc_err(status: i32) -> Vec<u8> {
    let mut resp = Vec::with_capacity(8);
    resp.extend_from_slice(&status.to_le_bytes());
    resp.extend_from_slice(&0u32.to_le_bytes());
    resp
}

#[cfg(target_os = "macos")]
fn xnu_process_exists(pid: u32) -> bool {
    unsafe {
        let mut info: [i32; 4096] = [0; 4096];
        let size = std::mem::size_of::<i32>() * info.len();
        let kr = libc::proc_pidinfo(
            pid as i32,
            libc::PROC_PIDTASKINFO,
            0,
            info.as_mut_ptr() as *mut libc::c_void,
            size as i32,
        );
        kr > 0
    }
}

#[cfg(not(target_os = "macos"))]
fn xnu_process_exists(_pid: u32) -> bool {
    true
}

#[cfg(target_os = "macos")]
const PROC_PIDLISTTHREADS: i32 = 4;

#[cfg(target_os = "macos")]
fn xnu_thread_exists(tid: u32) -> bool {
    unsafe {
        let pid = getpid();
        let mut buf: [u8; 32768] = [0; 32768];
        let count =
            libc::proc_pidinfo(pid, PROC_PIDLISTTHREADS, 0, buf.as_mut_ptr() as *mut libc::c_void, buf.len() as i32);
        if count <= 0 {
            return false;
        }
        let thread_count = (count as usize) / std::mem::size_of::<u64>();
        if thread_count == 0 {
            return false;
        }
        let thread_ids = std::slice::from_raw_parts(buf.as_ptr() as *const u64, thread_count);
        thread_ids.iter().any(|&t| t as u32 == tid)
    }
}

#[cfg(not(target_os = "macos"))]
fn xnu_thread_exists(_tid: u32) -> bool {
    true
}

#[cfg(target_os = "macos")]
fn getpid() -> i32 {
    unsafe { libc::getpid() }
}

#[cfg(not(target_os = "macos"))]
fn getpid() -> i32 {
    0
}

#[cfg(target_os = "macos")]
mod xnu_vm {
    pub const VM_PROT_NONE: i32 = 0x00;
    pub const VM_PROT_READ: i32 = 0x01;
    pub const VM_PROT_WRITE: i32 = 0x02;
    pub const VM_PROT_EXECUTE: i32 = 0x04;

    pub const VM_REGION_BASIC_INFO_64: i32 = 9;

    #[repr(C)]
    #[derive(Default)]
    pub struct VmRegionBasicInfo64 {
        pub protection: i32,
        pub max_protection: i32,
        pub inheritance: u32,
        pub shared: boolean_t,
        pub reserved: boolean_t,
        pub offset: u64,
        pub behavior: i32,
        pub user_wired_count: u16,
    }

    #[allow(non_camel_case_types)]
    pub type boolean_t = i32;

    extern "C" {
        fn mach_vm_region(
            target_task: u32,
            address: *mut u64,
            size: *mut u64,
            flavor: i32,
            info: *mut VmRegionBasicInfo64,
            info_cnt: *mut u32,
            object_name: *mut u32,
        ) -> i32;

        fn task_for_pid(target_tport: u32, pid: i32, t: *mut u32) -> i32;

        fn mach_task_self() -> u32;
        fn mach_port_deallocate(target_tport: u32, name: u32) -> i32;
    }

    const KERN_SUCCESS: i32 = 0;

    pub struct VmRegion {
        pub address: u64,
        pub size: u64,
        pub protection: i32,
        pub max_protection: i32,
        pub shared: bool,
        pub offset: u64,
    }

    pub fn query_vm_region(pid: u32, base_address: u64) -> Option<VmRegion> {
        unsafe {
            let self_task = mach_task_self();
            // Self queries neither require task_for_pid entitlement nor return an owned port.
            let (task, deallocate_task) = if pid == libc::getpid() as u32 {
                (self_task, false)
            } else {
                let mut task: u32 = 0;
                let kr = task_for_pid(self_task, pid as i32, &mut task);
                if kr != KERN_SUCCESS {
                    return None;
                }
                (task, true)
            };

            let mut addr = if base_address == 0 { 0x1000 } else { base_address };
            let mut size: u64 = 0;
            let mut info = VmRegionBasicInfo64::default();
            let mut info_cnt = std::mem::size_of::<VmRegionBasicInfo64>() as u32 / 4;
            let mut object_name: u32 = 0;

            let kr = mach_vm_region(
                task,
                &mut addr,
                &mut size,
                VM_REGION_BASIC_INFO_64,
                &mut info,
                &mut info_cnt,
                &mut object_name,
            );

            if deallocate_task {
                let _ = mach_port_deallocate(self_task, task);
            }

            if kr != KERN_SUCCESS {
                return None;
            }

            Some(VmRegion {
                address: addr,
                size,
                protection: info.protection,
                max_protection: info.max_protection,
                shared: info.shared != 0,
                offset: info.offset,
            })
        }
    }

    fn nt_protect_from_xnu(prot: i32) -> (u32, u32, u32) {
        let (state, protect, vtype) = if prot == VM_PROT_NONE {
            (0x10000u32, 0x01u32, 0x00020000u32)
        } else {
            let mut protect = 0u32;
            let mut vtype = 0x00020000u32;
            if prot & VM_PROT_READ != 0 {
                protect |= 0x02;
            }
            if prot & VM_PROT_WRITE != 0 {
                protect |= 0x04;
            }
            if prot & VM_PROT_EXECUTE != 0 {
                protect |= 0x20;
                vtype = 0x00040000;
            }
            (0x1000u32, protect, vtype)
        };

        (state, protect, vtype)
    }

    pub fn build_mbi_from_xnu(pid: u32, base_address: u64) -> Option<(u64, u64, u32, u32, u32, u32)> {
        let region = query_vm_region(pid, base_address)?;
        let (state, protect, vtype) = nt_protect_from_xnu(region.protection);
        let (_, alloc_protect, _) = nt_protect_from_xnu(region.max_protection);
        Some((region.address, region.size, state, protect, vtype, alloc_protect))
    }
}

fn handle_ipc_request(operation: u16, body: &[u8]) -> Vec<u8> {
    match operation {
        OP_NT_OPEN_PROCESS => handle_nt_open_process(body),
        OP_NT_OPEN_THREAD => handle_nt_open_thread(body),
        OP_NT_QUERY_SYSTEM_INFO => handle_nt_query_system_info(body),
        OP_NT_QUERY_INFORMATION_PROC => handle_nt_query_info_process(body),
        OP_NT_QUERY_OBJECT => handle_nt_query_object(body),
        OP_NT_SET_INFORMATION_THREAD => handle_nt_set_information_thread(body),
        OP_NT_CLOSE => handle_nt_close(body),
        OP_NT_QUERY_VIRTUAL_MEMORY => handle_nt_query_virtual_memory(body),
        OP_NT_DEVICE_IO_CONTROL => handle_nt_device_io_control(body),
        _ => {
            let mut resp = Vec::with_capacity(8);
            resp.extend_from_slice(&pack_i32(STATUS_NOT_IMPLEMENTED));
            resp.extend_from_slice(&pack_u32(0));
            resp
        },
    }
}

fn handle_nt_open_process(body: &[u8]) -> Vec<u8> {
    if body.len() < 12 {
        let mut resp = Vec::with_capacity(12);
        resp.extend_from_slice(&pack_i32(STATUS_INVALID_PARAMETER));
        resp.extend_from_slice(&pack_u64(0));
        return resp;
    }
    let desired_access = unpack_u32(body, 0);
    let _inherit = unpack_u32(body, 4);
    let pid = unpack_u32(body, 8);

    if !xnu_process_exists(pid) {
        let mut resp = Vec::with_capacity(12);
        resp.extend_from_slice(&pack_i32(STATUS_ACCESS_DENIED));
        resp.extend_from_slice(&pack_u64(0));
        return resp;
    }

    let handle = alloc_handle(pid, 0, desired_access, "Process");

    let mut resp = Vec::with_capacity(12);
    resp.extend_from_slice(&pack_i32(STATUS_SUCCESS));
    resp.extend_from_slice(&pack_u64(handle));
    resp
}

fn handle_nt_open_thread(body: &[u8]) -> Vec<u8> {
    if body.len() < 12 {
        let mut resp = Vec::with_capacity(12);
        resp.extend_from_slice(&pack_i32(STATUS_INVALID_PARAMETER));
        resp.extend_from_slice(&pack_u64(0));
        return resp;
    }
    let desired_access = unpack_u32(body, 0);
    let _inherit = unpack_u32(body, 4);
    let tid = unpack_u32(body, 8);

    if !xnu_thread_exists(tid) {
        let mut resp = Vec::with_capacity(12);
        resp.extend_from_slice(&pack_i32(STATUS_ACCESS_DENIED));
        resp.extend_from_slice(&pack_u64(0));
        return resp;
    }

    let handle = alloc_handle(0, tid, desired_access, "Thread");

    let mut resp = Vec::with_capacity(12);
    resp.extend_from_slice(&pack_i32(STATUS_SUCCESS));
    resp.extend_from_slice(&pack_u64(handle));
    resp
}

fn handle_nt_query_system_info(body: &[u8]) -> Vec<u8> {
    if body.len() < 8 {
        return ipc_err(STATUS_INVALID_PARAMETER);
    }
    let info_class = unpack_u32(body, 0);
    let buf_len = unpack_u32(body, 4);

    let handles = lock_handles();
    let process_count = handles.values().filter(|h| h.handle_type == "Process").count() as u32;

    match info_class {
        0x10 => {
            let needed: usize = process_count as usize * 60 + 8;
            if (buf_len as usize) < needed {
                let mut meta = Vec::with_capacity(12);
                meta.extend_from_slice(&STATUS_INFO_LENGTH_MISMATCH.to_le_bytes());
                meta.extend_from_slice(&pack_u32(needed as u32));
                meta.extend_from_slice(&pack_u32(0));
                return meta;
            }
            let mut data = Vec::with_capacity(8 + process_count as usize * 60);
            data.extend_from_slice(&pack_u32(process_count));
            data.extend_from_slice(&pack_u32(0));
            for h in handles.values().filter(|h| h.handle_type == "Process") {
                data.extend_from_slice(&pack_u32(next_handle_delta(&handles, h.pid)));
                data.extend_from_slice(&pack_u32(h.pid));
                data.extend_from_slice(&[0u8; 48]);
                data.extend_from_slice(&pack_u32(0));
            }
            ipc_ok(data.len() as u32, &data)
        },
        _ => {
            let data = vec![0u8; 4];
            ipc_ok(4, &data)
        },
    }
}

fn next_handle_delta(handles: &BTreeMap<u64, VirtualHandleEntry>, pid: u32) -> u32 {
    for (i, h) in handles.values().enumerate() {
        if h.pid == pid {
            return (i + 1) as u32;
        }
    }
    0
}

fn handle_nt_query_info_process(body: &[u8]) -> Vec<u8> {
    if body.len() < 12 {
        return ipc_err(STATUS_INVALID_PARAMETER);
    }
    let _handle = unpack_u64(body, 0);
    let info_class = unpack_u32(body, 8);

    match info_class {
        0x00 => {
            let mut data = vec![0u8; 48];
            data[0..4].copy_from_slice(&259i32.to_le_bytes());
            let pid = getpid() as u64;
            data[32..40].copy_from_slice(&pid.to_le_bytes());
            ipc_ok(48, &data)
        },
        0x07 => {
            let data = vec![0u8; 8];
            ipc_ok(8, &data)
        },
        0x1e => {
            let data = vec![0u8; 8];
            ipc_ok(8, &data)
        },
        0x1f => {
            let mut data = vec![0u8; 4];
            data[0..4].copy_from_slice(&1u32.to_le_bytes());
            ipc_ok(4, &data)
        },
        _ => ipc_err(STATUS_NOT_IMPLEMENTED),
    }
}

fn handle_nt_close(body: &[u8]) -> Vec<u8> {
    if body.len() < 8 {
        return pack_i32(STATUS_INVALID_HANDLE).to_vec();
    }
    let handle = unpack_u64(body, 0);
    let removed = lock_handles().remove(&handle).is_some();
    let status = if removed { STATUS_SUCCESS } else { STATUS_INVALID_HANDLE };
    pack_i32(status).to_vec()
}

fn handle_nt_device_io_control(body: &[u8]) -> Vec<u8> {
    if body.len() < 20 {
        let mut resp = Vec::with_capacity(12);
        resp.extend_from_slice(&pack_i32(STATUS_INVALID_PARAMETER));
        resp.extend_from_slice(&pack_u32(0));
        resp.extend_from_slice(&pack_u32(0));
        return resp;
    }
    let _handle = unpack_u64(body, 0);
    let ioctl_code = unpack_u32(body, 8);
    let input_len = unpack_u32(body, 12);
    let output_len = unpack_u32(body, 16);

    let _input_data = if body.len() > 20 && input_len as usize <= body.len() - 20 {
        &body[20..20 + input_len as usize]
    } else {
        &body[20..]
    };

    match ioctl_code {
        0x00090000..=0x0009FFFF => {
            let mut resp = Vec::with_capacity(12 + output_len as usize);
            resp.extend_from_slice(&pack_i32(STATUS_SUCCESS));
            resp.extend_from_slice(&pack_u32(0));
            resp.extend_from_slice(&pack_u32(output_len));
            if output_len > 0 {
                resp.extend_from_slice(&vec![0u8; output_len as usize]);
            }
            resp
        },
        _ => {
            let mut resp = Vec::with_capacity(12);
            resp.extend_from_slice(&pack_i32(STATUS_NOT_IMPLEMENTED));
            resp.extend_from_slice(&pack_u32(0));
            resp.extend_from_slice(&pack_u32(0));
            resp
        },
    }
}

fn handle_nt_query_virtual_memory(body: &[u8]) -> Vec<u8> {
    if body.len() < 20 {
        return ipc_err(STATUS_INVALID_PARAMETER);
    }
    let process_handle = unpack_u64(body, 0);
    let base_address = unpack_u64(body, 8);
    let info_class = unpack_u32(body, 16);

    match info_class {
        0x00 => {
            let handles = lock_handles();
            let pid = handles.get(&process_handle).map(|h| h.pid).unwrap_or(0);

            let (region_base, region_size, state, protect, vtype, alloc_protect) =
                if cfg!(target_os = "macos") && pid > 0 {
                    match xnu_vm::build_mbi_from_xnu(pid, base_address) {
                        Some(r) => r,
                        None => return ipc_err(0xC0000018_u32 as i32),
                    }
                } else {
                    (base_address, 0x10000u64, 0x1000u32, 0x04u32, 0x00020000u32, 0x04u32)
                };

            let mut data = vec![0u8; 48];
            data[0..8].copy_from_slice(&region_base.to_le_bytes());
            data[8..16].copy_from_slice(&region_base.to_le_bytes());
            data[16..20].copy_from_slice(&alloc_protect.to_le_bytes());
            data[24..32].copy_from_slice(&region_size.to_le_bytes());
            data[32..36].copy_from_slice(&state.to_le_bytes());
            data[36..40].copy_from_slice(&protect.to_le_bytes());
            data[40..44].copy_from_slice(&vtype.to_le_bytes());
            ipc_ok(48, &data)
        },
        _ => ipc_err(STATUS_NOT_IMPLEMENTED),
    }
}

fn handle_nt_query_object(body: &[u8]) -> Vec<u8> {
    if body.len() < 12 {
        return ipc_err(STATUS_INVALID_PARAMETER);
    }
    let handle = unpack_u64(body, 0);
    let info_class = unpack_u32(body, 8);

    match info_class {
        0x01 => {
            let type_name = {
                let handles = lock_handles();
                match handles.get(&handle) {
                    Some(entry) => entry.handle_type.clone(),
                    None => "Unknown".to_string(),
                }
            };

            let type_utf16: Vec<u16> = format!("{}\0", type_name).encode_utf16().collect();
            let mut data = Vec::with_capacity(type_utf16.len() * 2);
            for c in &type_utf16 {
                data.extend_from_slice(&c.to_le_bytes());
            }
            ipc_ok(data.len() as u32, &data)
        },
        0x02 => {
            let data = vec![0u8, 0u8];
            ipc_ok(2, &data)
        },
        _ => ipc_err(STATUS_NOT_IMPLEMENTED),
    }
}

fn handle_nt_set_information_thread(body: &[u8]) -> Vec<u8> {
    if body.len() < 12 {
        return pack_i32(STATUS_INVALID_PARAMETER).to_vec();
    }
    let _thread_handle = unpack_u64(body, 0);
    let info_class = unpack_u32(body, 8);

    match info_class {
        0x11 => pack_i32(STATUS_SUCCESS).to_vec(),
        _ => pack_i32(STATUS_NOT_IMPLEMENTED).to_vec(),
    }
}

fn handle_client(mut stream: TcpStream) {
    let active = IPC_ACTIVE_CLIENTS.fetch_add(1, Ordering::Relaxed);
    if active >= MAX_CONCURRENT_CLIENTS as u32 {
        IPC_ACTIVE_CLIENTS.fetch_sub(1, Ordering::Relaxed);
        return;
    }

    let _ = stream.set_read_timeout(Some(std::time::Duration::from_secs(30)));
    let _ = stream.set_write_timeout(Some(std::time::Duration::from_secs(10)));

    let result = handle_client_inner(&mut stream);

    IPC_ACTIVE_CLIENTS.fetch_sub(1, Ordering::Relaxed);
    let _ = result;
}

fn handle_client_inner(stream: &mut TcpStream) -> Result<(), ()> {
    loop {
        let header_buf = read_exact_u8(stream, IPC_HEADER_SIZE).map_err(|_| ())?;

        if header_buf.len() < IPC_HEADER_SIZE {
            return Err(());
        }

        let magic = u32::from_le_bytes(header_buf[0..4].try_into().unwrap_or([0; 4]));
        let version = u16::from_le_bytes(header_buf[4..6].try_into().unwrap_or([0; 2]));
        let operation = u16::from_le_bytes(header_buf[6..8].try_into().unwrap_or([0; 2]));
        let _request_id = u32::from_le_bytes(header_buf[8..12].try_into().unwrap_or([0; 4]));
        let body_size = u32::from_le_bytes(header_buf[12..16].try_into().unwrap_or([0; 4]));

        if magic != MS_IPC_MAGIC || version != MS_IPC_VERSION {
            let _ = stream.write_all(&0xC0000001u32.to_le_bytes());
            return Err(());
        }

        let body = if body_size > 0 && body_size <= 65536 {
            read_exact_u8(stream, body_size as usize).map_err(|_| ())?
        } else {
            Vec::new()
        };

        let response = handle_ipc_request(operation, &body);

        let resp_body_size = response.len() as u32;
        let resp_request_id = NEXT_REQUEST_ID.fetch_add(1, Ordering::Relaxed);

        let mut out = Vec::with_capacity(IPC_HEADER_SIZE + response.len());
        out.extend_from_slice(&MS_IPC_MAGIC.to_le_bytes());
        out.extend_from_slice(&MS_IPC_VERSION.to_le_bytes());
        out.extend_from_slice(&operation.to_le_bytes());
        out.extend_from_slice(&resp_request_id.to_le_bytes());
        out.extend_from_slice(&resp_body_size.to_le_bytes());
        out.extend_from_slice(&response);

        stream.write_all(&out).map_err(|_| ())?;
    }
}

pub fn handle_ipc_status(_body: &serde_json::Map<String, Value>) -> Value {
    json!({
        "ok": true,
        "bind_addr": crate::kernel_translation::ipc_bridge::IPC_BIND_ADDR,
        "running": IPC_LISTENER_RUNNING.load(Ordering::Relaxed),
        "virtual_handles": lock_handles().len(),
    })
}

pub fn handle_ipc_handles(_body: &serde_json::Map<String, Value>) -> Value {
    let handles = lock_handles();
    let entries: Vec<Value> = handles
        .values()
        .map(|h| {
            json!({
                "handle": format!("0x{:08X}", h.handle),
                "pid": h.pid,
                "tid": h.tid,
                "access_mask": format!("0x{:08X}", h.access_mask),
                "type": h.handle_type,
            })
        })
        .collect();
    json!({
        "ok": true,
        "count": entries.len(),
        "handles": entries,
    })
}

pub fn start_ipc_listener() -> Result<(), String> {
    if IPC_LISTENER_RUNNING.load(Ordering::Relaxed) {
        return Ok(());
    }

    let listener =
        TcpListener::bind(IPC_BIND_ADDR).map_err(|e| format!("IPC TCP bind failed at {}: {}", IPC_BIND_ADDR, e))?;

    IPC_LISTENER_RUNNING.store(true, Ordering::Relaxed);
    *IPC_LISTENER.lock().unwrap() = Some(listener.try_clone().map_err(|e| format!("clone failed: {}", e))?);

    thread::spawn(move || {
        for stream in listener.incoming() {
            if !IPC_LISTENER_RUNNING.load(Ordering::Relaxed) {
                break;
            }
            match stream {
                Ok(stream) => {
                    if IPC_ACTIVE_CLIENTS.load(Ordering::Relaxed) < MAX_CONCURRENT_CLIENTS as u32 {
                        thread::spawn(|| handle_client(stream));
                    }
                },
                Err(_) => {
                    if !IPC_LISTENER_RUNNING.load(Ordering::Relaxed) {
                        break;
                    }
                },
            }
        }
    });

    Ok(())
}

pub fn stop_ipc_listener() -> Value {
    IPC_LISTENER_RUNNING.store(false, Ordering::Relaxed);
    let listener = IPC_LISTENER.lock().unwrap().take();
    drop(listener);
    json!({"ok": true, "stopped": true, "active_clients": IPC_ACTIVE_CLIENTS.load(Ordering::Relaxed)})
}

pub const IPC_BIND_ADDR: &str = "127.0.0.1:19384";

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_alloc_handle_increments() {
        let h1 = alloc_handle(100, 0, 0x1F0FFF, "Process");
        let h2 = alloc_handle(200, 0, 0x1F0FFF, "Process");

        assert!(h1 >= 0x100);
        assert!(h2 > h1);

        let handles = lock_handles();
        assert!(handles.contains_key(&h1));
        assert!(handles.contains_key(&h2));
        assert_eq!(handles[&h1].pid, 100);
        assert_eq!(handles[&h2].pid, 200);
    }

    #[test]
    fn test_nt_open_process_returns_handle() {
        let mut req = Vec::new();
        req.extend_from_slice(&0x1F0FFFu32.to_le_bytes());
        req.extend_from_slice(&0u32.to_le_bytes());
        req.extend_from_slice(&getpid().to_le_bytes());

        let resp = handle_nt_open_process(&req);
        assert_eq!(resp.len(), 12);
        let status = i32::from_le_bytes(resp[0..4].try_into().unwrap());
        assert_eq!(status, STATUS_SUCCESS);
        let handle = u64::from_le_bytes(resp[4..12].try_into().unwrap());
        assert!(handle >= 0x100);
    }

    #[test]
    fn test_nt_close_removes_handle() {
        let h = alloc_handle(42, 0, 0, "Process");

        let mut req = Vec::new();
        req.extend_from_slice(&h.to_le_bytes());

        let resp = handle_nt_close(&req);
        let status = i32::from_le_bytes(resp[0..4].try_into().unwrap());
        assert_eq!(status, STATUS_SUCCESS);

        let resp2 = handle_nt_close(&req);
        let status2 = i32::from_le_bytes(resp2[0..4].try_into().unwrap());
        assert_eq!(status2, STATUS_INVALID_HANDLE);
    }

    #[test]
    fn test_nt_query_info_process_debug_flags() {
        let mut req = Vec::new();
        req.extend_from_slice(&0x1234u64.to_le_bytes());
        req.extend_from_slice(&0x1Fu32.to_le_bytes());
        req.extend_from_slice(&4u32.to_le_bytes());

        let resp = handle_nt_query_info_process(&req);
        let status = i32::from_le_bytes(resp[0..4].try_into().unwrap());
        assert_eq!(status, STATUS_SUCCESS);
        let return_len = u32::from_le_bytes(resp[4..8].try_into().unwrap());
        assert_eq!(return_len, 4);
        let flags = u32::from_le_bytes(resp[8..12].try_into().unwrap());
        assert_eq!(flags, 1);
    }

    #[test]
    fn test_nt_device_io_control_fsctl_ioctl_succeeds() {
        let mut req = Vec::new();
        req.extend_from_slice(&0x5678u64.to_le_bytes());
        req.extend_from_slice(&0x00090000u32.to_le_bytes());
        req.extend_from_slice(&0u32.to_le_bytes());
        req.extend_from_slice(&64u32.to_le_bytes());

        let resp = handle_nt_device_io_control(&req);
        let status = i32::from_le_bytes(resp[0..4].try_into().unwrap());
        assert_eq!(status, STATUS_SUCCESS);
        assert!(resp.len() >= 12);
    }

    #[test]
    fn test_ipc_header_roundtrip() {
        VIRTUAL_HANDLES.lock().unwrap().clear();

        let mut req = Vec::new();
        req.extend_from_slice(&MS_IPC_MAGIC.to_le_bytes());
        req.extend_from_slice(&MS_IPC_VERSION.to_le_bytes());
        req.extend_from_slice(&OP_NT_CLOSE.to_le_bytes());
        req.extend_from_slice(&1u32.to_le_bytes());
        req.extend_from_slice(&8u32.to_le_bytes());

        req.extend_from_slice(&0x1234u64.to_le_bytes());

        assert_eq!(req.len(), 24);

        let op = u16::from_le_bytes(req[6..8].try_into().unwrap());
        assert_eq!(op, OP_NT_CLOSE);

        let body_size = u32::from_le_bytes(req[12..16].try_into().unwrap());
        assert_eq!(body_size, 8);
    }

    #[test]
    fn test_protocol_constants_match_dll() {
        assert_eq!(MS_IPC_MAGIC, 0x4D534B54u32);
        assert_eq!(MS_IPC_VERSION, 1u16);
        assert_eq!(IPC_HEADER_SIZE, 16usize);
        assert_eq!(OP_NT_OPEN_PROCESS, 0x0001u16);
        assert_eq!(OP_NT_OPEN_THREAD, 0x0002u16);
        assert_eq!(OP_NT_QUERY_OBJECT, 0x0005u16);
        assert_eq!(OP_NT_SET_INFORMATION_THREAD, 0x0006u16);
        assert_eq!(OP_NT_CLOSE, 0x0007u16);
        assert_eq!(OP_NT_QUERY_VIRTUAL_MEMORY, 0x0009u16);
        assert_eq!(OP_NT_DEVICE_IO_CONTROL, 0x000Du16);
        assert_eq!(STATUS_SUCCESS, 0i32);
        assert_eq!(STATUS_NOT_IMPLEMENTED, 0xC0000002_u32 as i32);
        assert_eq!(STATUS_INVALID_PARAMETER, 0xC000000D_u32 as i32);
        assert_eq!(STATUS_INVALID_HANDLE, 0xC0000008_u32 as i32);
        assert_eq!(STATUS_ACCESS_DENIED, 0xC0000022_u32 as i32);
    }

    #[test]
    fn test_nt_close_error_path_uses_pack() {
        let resp = handle_nt_close(&[0u8; 4]);
        assert_eq!(resp.len(), 4);
        let status = i32::from_le_bytes(resp[0..4].try_into().unwrap());
        assert_eq!(status, STATUS_INVALID_HANDLE);
    }

    #[test]
    fn test_nt_device_io_control_unknown_ioctl_returns_not_implemented() {
        let mut req = Vec::new();
        req.extend_from_slice(&0x5678u64.to_le_bytes());
        req.extend_from_slice(&0xDEAD0000u32.to_le_bytes());
        req.extend_from_slice(&0u32.to_le_bytes());
        req.extend_from_slice(&0u32.to_le_bytes());

        let resp = handle_nt_device_io_control(&req);
        let status = i32::from_le_bytes(resp[0..4].try_into().unwrap());
        assert_eq!(status, STATUS_NOT_IMPLEMENTED);
    }

    #[test]
    fn test_nt_query_virtual_memory_mapped_address_returns_success() {
        let h = alloc_handle(getpid() as u32, 0, 0x1F0FFF, "Process");
        let marker = 0u64;
        let base_address = (&marker as *const u64) as usize as u64;

        let mut req = Vec::new();
        req.extend_from_slice(&h.to_le_bytes());
        req.extend_from_slice(&base_address.to_le_bytes());
        req.extend_from_slice(&0x00u32.to_le_bytes());
        req.extend_from_slice(&48u32.to_le_bytes());

        let resp = handle_nt_query_virtual_memory(&req);
        let status = i32::from_le_bytes(resp[0..4].try_into().unwrap());
        assert_eq!(status, STATUS_SUCCESS);
        let return_len = u32::from_le_bytes(resp[4..8].try_into().unwrap());
        assert_eq!(return_len, 48);
        assert_eq!(resp.len(), 8 + 48);
        let region_base = u64::from_le_bytes(resp[8..16].try_into().unwrap());
        let region_size = u64::from_le_bytes(resp[32..40].try_into().unwrap());
        assert!(region_size > 0);
        assert!(base_address >= region_base);
        assert!(base_address - region_base < region_size);
    }

    #[test]
    fn test_nt_query_object_type_info_returns_utf16() {
        let h = alloc_handle(1, 0, 0, "Process");

        let mut req = Vec::new();
        req.extend_from_slice(&h.to_le_bytes());
        req.extend_from_slice(&0x01u32.to_le_bytes());
        req.extend_from_slice(&24u32.to_le_bytes());

        let resp = handle_nt_query_object(&req);
        let status = i32::from_le_bytes(resp[0..4].try_into().unwrap());
        assert_eq!(status, STATUS_SUCCESS);
        assert!(resp.len() > 8);
        let type_name: Vec<u16> = "Process\0".encode_utf16().collect();
        let expected_bytes: Vec<u8> = type_name.iter().flat_map(|c| c.to_le_bytes()).collect();
        assert_eq!(&resp[8..], expected_bytes.as_slice());
    }

    #[test]
    fn test_nt_query_object_data_info_returns_two_bytes() {
        let mut req = Vec::new();
        req.extend_from_slice(&0x100u64.to_le_bytes());
        req.extend_from_slice(&0x02u32.to_le_bytes());
        req.extend_from_slice(&2u32.to_le_bytes());

        let resp = handle_nt_query_object(&req);
        let status = i32::from_le_bytes(resp[0..4].try_into().unwrap());
        assert_eq!(status, STATUS_SUCCESS);
        let return_len = u32::from_le_bytes(resp[4..8].try_into().unwrap());
        assert_eq!(return_len, 2);
        assert_eq!(resp.len(), 10);
    }

    #[test]
    fn test_nt_set_information_thread_hide_from_debugger() {
        let mut req = Vec::new();
        req.extend_from_slice(&0x100u64.to_le_bytes());
        req.extend_from_slice(&0x11u32.to_le_bytes());
        req.extend_from_slice(&4u32.to_le_bytes());

        let resp = handle_nt_set_information_thread(&req);
        let status = i32::from_le_bytes(resp[0..4].try_into().unwrap());
        assert_eq!(status, STATUS_SUCCESS);
    }
}
