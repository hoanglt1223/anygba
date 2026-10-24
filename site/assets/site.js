"use strict";
(() => {
  const root = document.documentElement;
  const dark = () => root.dataset.theme === "dark" ||
    (!root.dataset.theme && matchMedia("(prefers-color-scheme: dark)").matches);
  const theme = document.querySelector(".theme-toggle");
  const sync = () => theme.setAttribute("aria-pressed", String(dark()));
  sync();
  theme.addEventListener("click", () => {
    root.dataset.theme = dark() ? "light" : "dark";
    localStorage.setItem("theme", root.dataset.theme);
    sync();
  });

  const toggle = document.querySelector(".nav-toggle");
  const nav = document.getElementById("nav");
  toggle.addEventListener("click", () => {
    const open = nav.classList.toggle("open");
    toggle.setAttribute("aria-expanded", String(open));
  });
})();
