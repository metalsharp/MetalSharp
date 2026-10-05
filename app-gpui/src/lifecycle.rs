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

/// Children inherit the spawning thread's signal mask. GPUI work runs on GCD
/// worker threads that block signals, so without this a spawned backend or
/// script never receives SIGTERM (stops always fall through to SIGKILL).
pub fn unmask_child_signals(command: &mut std::process::Command) {
    #[cfg(unix)]
    unsafe {
        use std::os::unix::process::CommandExt;
        command.pre_exec(|| {
            let mut set: libc::sigset_t = std::mem::zeroed();
            libc::sigemptyset(&mut set);
            libc::pthread_sigmask(libc::SIG_SETMASK, &set, std::ptr::null_mut());
            Ok(())
        });
    }
}
