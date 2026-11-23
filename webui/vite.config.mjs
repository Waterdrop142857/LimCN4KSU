import { resolve } from "node:path";
import { defineConfig } from "vite";

// Builds the module WebUI straight into module/webroot.
//
// Emit a classic IIFE for compatibility with managers that load the WebUI from
// a file origin. Current KernelSU may serve it through WebViewAssetLoader over
// HTTPS. File names stay fixed: no hashes, no absolute paths.
export default defineConfig({
  root: import.meta.dirname,
  base: "./",
  build: {
    outDir: resolve(import.meta.dirname, "..", "module", "webroot"),
    emptyOutDir: true,
    assetsDir: "",
    target: "es2019",
    cssCodeSplit: false,
    sourcemap: false,
    rollupOptions: {
      input: resolve(import.meta.dirname, "index.html"),
      output: {
        format: "iife",
        inlineDynamicImports: true,
        entryFileNames: "app.js",
        chunkFileNames: "[name].js",
        assetFileNames: (info) =>
          info.names && info.names.some((name) => name.endsWith(".css"))
            ? "app.css"
            : "[name][extname]",
      },
    },
  },
  plugins: [
    {
      name: "bybus-classic-script",
      transformIndexHtml(html) {
        return html
          .replace(
            /<script type="module" crossorigin src="\.\/app\.js"><\/script>/,
            '<script src="./app.js"></script>',
          )
          .replace(/<link rel="stylesheet" crossorigin href="\.\/app\.css">/, '<link rel="stylesheet" href="./app.css">');
      },
    },
  ],
});
