declare module "*.vue" {
  import type { DefineComponent } from "vue";
  const component: DefineComponent<object, object, unknown>;
  export default component;
}

declare module "~icons/*" {
  import type { Component } from "vue";
  const component: Component;
  export default component;
}
