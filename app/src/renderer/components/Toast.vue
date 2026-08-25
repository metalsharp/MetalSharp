<script setup lang="ts">
import { useToast } from "../composables/useToast";

const { toasts } = useToast();
</script>

<template>
  <div class="toast-container">
    <TransitionGroup name="toast">
      <div v-for="t in toasts" :key="t.id" class="toast" :class="t.type">
        <div class="toast-text">{{ t.text }}</div>
        <div v-if="t.type === 'download' || t.progress !== undefined" class="toast-progress-track">
          <div class="toast-progress" :style="{ width: `${Math.round((t.progress ?? 0) * 100)}%` }"></div>
        </div>
      </div>
    </TransitionGroup>
  </div>
</template>

<style scoped>
.toast-container {
  position: fixed;
  bottom: 20px;
  right: 20px;
  z-index: 9999;
  display: flex;
  flex-direction: column;
  gap: 8px;
  pointer-events: none;
}
.toast {
  padding: 10px 18px;
  border-radius: var(--radius-md);
  font-size: 13px;
  pointer-events: auto;
  max-width: 360px;
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.2);
}
.toast-text {
  line-height: 1.35;
}
.toast-progress-track {
  width: 100%;
  height: 4px;
  margin-top: 8px;
  overflow: hidden;
  border-radius: 99px;
  background: color-mix(in srgb, var(--text-primary) 18%, transparent);
}
.toast-progress {
  height: 100%;
  border-radius: inherit;
  background: var(--accent);
  transition: width 120ms linear;
}
.toast.download {
  min-width: 280px;
  color: var(--text-primary);
  background: var(--bg-card);
  border: 1px solid var(--border);
}
.toast.success {
  background: var(--success);
  color: #fff;
}
.toast.error {
  background: var(--error);
  color: #fff;
}
.toast-enter-active {
  transition: all 0.2s ease;
}
.toast-leave-active {
  transition: all 0.2s ease;
}
.toast-enter-from {
  opacity: 0;
  transform: translateY(12px);
}
.toast-leave-to {
  opacity: 0;
  transform: translateX(40px);
}
</style>
