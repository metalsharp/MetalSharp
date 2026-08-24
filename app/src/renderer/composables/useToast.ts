import { ref } from "vue";

interface ToastMessage {
  id: number;
  text: string;
  type: "success" | "error" | "download";
  progress?: number;
  persistent?: boolean;
}

const toasts = ref<ToastMessage[]>([]);
let nextId = 0;
const DEFAULT_TOAST_MS = 4000;

export function useToast() {
  function show(text: string, type: "success" | "error" = "success", durationMs?: number) {
    const id = nextId++;
    toasts.value.push({ id, text, type });
    setTimeout(() => {
      toasts.value = toasts.value.filter((t) => t.id !== id);
    }, durationMs ?? DEFAULT_TOAST_MS);
    return id;
  }

  function showDownload(text: string, progress = 0) {
    const id = nextId++;
    toasts.value.push({ id, text, type: "download", progress, persistent: true });
    return id;
  }

  function updateDownload(id: number, text: string, progress: number) {
    const toast = toasts.value.find((item) => item.id === id);
    if (toast) {
      toast.text = text;
      toast.progress = Math.max(0, Math.min(1, progress));
    }
  }

  function finishDownload(id: number, text: string, success: boolean) {
    const toast = toasts.value.find((item) => item.id === id);
    if (!toast) return;
    toast.text = text;
    toast.type = success ? "success" : "error";
    toast.progress = success ? 1 : toast.progress;
    toast.persistent = false;
    setTimeout(
      () => {
        toasts.value = toasts.value.filter((item) => item.id !== id);
      },
      success ? 1800 : 6000,
    );
  }

  return { toasts, show, showDownload, updateDownload, finishDownload };
}
