//! Connected-app signal shutdown. Signal handlers only set a lock-free flag;
//! GPUI performs normal quit on its main thread so owned-child RAII can run.
use std::sync::atomic::{AtomicBool, Ordering};
static QUIT_REQUESTED: AtomicBool = AtomicBool::new(false);
extern "C" fn request_quit(_signal: libc::c_int) {
    QUIT_REQUESTED.store(true, Ordering::Relaxed);
}
pub fn quit_requested() -> bool {
    QUIT_REQUESTED.load(Ordering::Relaxed)
}
pub fn install_connected_signal_handlers() -> anyhow::Result<()> {
    let mut action: libc::sigaction = unsafe { std::mem::zeroed() };
    action.sa_sigaction = request_quit as *const () as usize;
    action.sa_flags = libc::SA_RESTART;
    unsafe {
        libc::sigemptyset(&mut action.sa_mask);
    }
    let mut old_term: libc::sigaction = unsafe { std::mem::zeroed() };
    if unsafe { libc::sigaction(libc::SIGTERM, &action, &mut old_term) } != 0 {
        anyhow::bail!("Could not register connected-app shutdown handler");
    }
    if unsafe { libc::sigaction(libc::SIGINT, &action, std::ptr::null_mut()) } != 0 {
        unsafe {
            libc::sigaction(libc::SIGTERM, &old_term, std::ptr::null_mut());
        }
        anyhow::bail!("Could not register connected-app interrupt handler");
    }
    Ok(())
}
