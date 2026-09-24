<script setup lang="ts">
import { computed } from "vue";
import { useI18n } from "vue-i18n";
import { localeOptions, setAppLocale } from "../i18n";

const props = withDefaults(
  defineProps<{
    compact?: boolean;
  }>(),
  { compact: false },
);
const { locale, t } = useI18n();
const selectedLocale = computed({
  get: () => locale.value,
  set: (value: string) => setAppLocale(value),
});
</script>

<template>
  <label class="language-picker" :class="{ compact: props.compact }">
    <span class="language-picker-label">{{ t("language.label") }}</span>
    <select v-model="selectedLocale" :aria-label="t('language.choose')">
      <option v-for="option in localeOptions" :key="option.code" :value="option.code">
        {{ option.native }}
      </option>
    </select>
  </label>
</template>

<style scoped>
.language-picker {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  color: rgba(255, 255, 255, 0.76);
  font-size: 12px;
}
.language-picker-label {
  font-weight: 650;
  letter-spacing: 0.02em;
}
.language-picker select {
  min-width: 150px;
  border: 1px solid rgba(255, 255, 255, 0.16);
  border-radius: 7px;
  padding: 6px 28px 6px 9px;
  background: rgba(16, 19, 22, 0.92);
  color: #fff;
  font: inherit;
}
.language-picker.compact select {
  min-width: 132px;
}
</style>
