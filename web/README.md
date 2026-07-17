# Codex Micro Web Simulator

An interactive browser recreation of the Codex Micro controller. The interface is built with HTML, CSS, React, and Lucide icons; no static device image is used in the simulator itself.

## Interactions

- Toggle six thread keys by clicking them or pressing `1`–`6`.
- Start or stop voice capture with the microphone key or `R`.
- Drag, scroll, or use arrow keys on both rotary controls.
- Trigger fast mode, approve, decline, and branch actions.
- Toggle device connection and the Agent layer.
- Use the full interface on pointer, keyboard, and touch devices.

## Development

Requires Node.js `>=22.13.0`.

```bash
npm install
npm run dev
npm run build
node --test tests/rendered-html.test.mjs
```

The site is packaged for OpenAI Sites through `.openai/hosting.json` and vinext.
