//! System-wide shortcuts equivalent to Electron's `globalShortcut` registrations.
//! Carbon hot keys are delivered through the application event loop and need no
//! Accessibility permission. Handlers only set flags; the UI tick loop acts on them.
use std::sync::atomic::{AtomicBool, Ordering};

static FORCE_QUIT_GAMES: AtomicBool = AtomicBool::new(false);

pub fn take_force_quit_request() -> bool {
    FORCE_QUIT_GAMES.swap(false, Ordering::Relaxed)
}

#[cfg(target_os = "macos")]
mod carbon {
    use std::ffi::c_void;

    #[repr(C)]
    pub struct EventTypeSpec {
        pub event_class: u32,
        pub event_kind: u32,
    }
    #[repr(C)]
    #[derive(Default)]
    pub struct EventHotKeyId {
        pub signature: u32,
        pub id: u32,
    }

    pub type Handler = extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> i32;

    #[link(name = "Carbon", kind = "framework")]
    unsafe extern "C" {
        pub fn GetApplicationEventTarget() -> *mut c_void;
        pub fn InstallEventHandler(
            target: *mut c_void,
            handler: Handler,
            num_types: usize,
            list: *const EventTypeSpec,
            user_data: *mut c_void,
            out_ref: *mut *mut c_void,
        ) -> i32;
        pub fn RegisterEventHotKey(
            key_code: u32,
            modifiers: u32,
            id: EventHotKeyId,
            target: *mut c_void,
            options: u32,
            out_ref: *mut *mut c_void,
        ) -> i32;
        pub fn GetEventParameter(
            event: *mut c_void,
            name: u32,
            desired_type: u32,
            actual_type: *mut u32,
            buffer_size: usize,
            actual_size: *mut usize,
            data: *mut c_void,
        ) -> i32;
    }

    pub const K_EVENT_CLASS_KEYBOARD: u32 = u32::from_be_bytes(*b"keyb");
    pub const K_EVENT_HOT_KEY_PRESSED: u32 = 5;
    pub const K_EVENT_PARAM_DIRECT_OBJECT: u32 = u32::from_be_bytes(*b"----");
    pub const TYPE_EVENT_HOT_KEY_ID: u32 = u32::from_be_bytes(*b"hkid");
    pub const SIGNATURE: u32 = u32::from_be_bytes(*b"MTSP");
    pub const CMD_KEY: u32 = 1 << 8;
    pub const OPTION_KEY: u32 = 1 << 11;
    pub const KVK_ANSI_Q: u32 = 0x0C;
    pub const FORCE_QUIT_ID: u32 = 1;
}

#[cfg(target_os = "macos")]
extern "C" fn on_hot_key(
    _: *mut std::ffi::c_void,
    event: *mut std::ffi::c_void,
    _: *mut std::ffi::c_void,
) -> i32 {
    use carbon::*;
    let mut id = EventHotKeyId::default();
    let status = unsafe {
        GetEventParameter(
            event,
            K_EVENT_PARAM_DIRECT_OBJECT,
            TYPE_EVENT_HOT_KEY_ID,
            std::ptr::null_mut(),
            std::mem::size_of::<EventHotKeyId>(),
            std::ptr::null_mut(),
            (&mut id as *mut EventHotKeyId).cast(),
        )
    };
    if status == 0 && id.signature == SIGNATURE && id.id == FORCE_QUIT_ID {
        FORCE_QUIT_GAMES.store(true, Ordering::Relaxed);
    }
    0
}

/// Register Cmd+Option+Q ("quit playing anytime", Electron `registerForceQuitGamesShortcut`).
pub fn register() {
    #[cfg(target_os = "macos")]
    unsafe {
        use carbon::*;
        let target = GetApplicationEventTarget();
        let spec = EventTypeSpec {
            event_class: K_EVENT_CLASS_KEYBOARD,
            event_kind: K_EVENT_HOT_KEY_PRESSED,
        };
        let mut handler = std::ptr::null_mut();
        if InstallEventHandler(
            target,
            on_hot_key,
            1,
            &spec,
            std::ptr::null_mut(),
            &mut handler,
        ) != 0
        {
            eprintln!("MetalSharp force-quit games shortcut handler not installed");
            return;
        }
        let mut hot_key = std::ptr::null_mut();
        let status = RegisterEventHotKey(
            KVK_ANSI_Q,
            CMD_KEY | OPTION_KEY,
            EventHotKeyId {
                signature: SIGNATURE,
                id: FORCE_QUIT_ID,
            },
            target,
            0,
            &mut hot_key,
        );
        if status != 0 {
            eprintln!("MetalSharp force-quit games shortcut not registered: Command+Option+Q");
        }
    }
}
