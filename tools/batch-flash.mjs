#!/usr/bin/env node
// Batch-flash + label one ESP32-CAM at a time.
// ────────────────────────────────────────────
// Each unit needs a UNIQUE provisioning PIN printed on its label (the out-of-band secret that seals
// the BLE provisioning payload — see README "Security notes"). The reference build bakes "123456";
// this tool mints a random PIN per board, compiles it in via -DCAGI_PROV_PIN, flashes, then reads the
// board back over serial to capture the EXACT MAC suffix it advertises ("CommandAGI Cam XXXX") so the
// printed label matches what the app shows during pairing. Every unit is appended to labels.csv and
// (optionally) rendered into a printable labels.html sheet.
//
// Usage (from apps/clients/firmware/ESP32-32S-OV3660-cam):
//   node tools/batch-flash.mjs                 # one board: random PIN, auto-detect port
//   node tools/batch-flash.mjs --port /dev/ttyUSB0
//   node tools/batch-flash.mjs --pin 482913    # force a specific PIN (else random 6 digits)
//   node tools/batch-flash.mjs --env esp32cam --labels labels.csv --html
//   node tools/batch-flash.mjs --html-only     # just (re)render labels.html from the CSV, no flash
//   node tools/batch-flash.mjs --loop          # BATCH: flash board → wait for it to be unplugged →
//                                                # auto-detect the next one → flash → … (Ctrl-C to stop).
//                                                # A fresh random PIN per board; can't pin a --port.
//   node tools/batch-flash.mjs --record https://api.commandagi.com   # ALSO mirror into the D1 ledger
//                                                # (auto-loads the service secret from the repo .env —
//                                                # API_SERVICE_SECRET, else legacy EDGE_SERVICE_SECRET;
//                                                # or set API_SERVICE_SECRET in the env. Best-effort.)
//   node tools/batch-flash.mjs --loop --html --record https://api.commandagi.com   # the full batch run
//
// Flow per board (single shot): plug it in (GPIO0→GND for the first flash on AI-Thinker), run the
// command, wait for "✓ labeled", unplug, repeat. With --loop you run the command ONCE and just keep
// swapping boards. Keep labels.csv with the batch — it's the PIN ledger.

import { spawn } from "node:child_process";
import {
  accessSync,
  appendFileSync,
  constants as fsConstants,
  existsSync,
  readFileSync,
  readdirSync,
  writeFileSync,
  mkdirSync,
} from "node:fs";
import { randomInt } from "node:crypto";
import { dirname, resolve } from "node:path";
import { hostname, userInfo } from "node:os";

function arg(name, fallback) {
  const i = process.argv.indexOf(`--${name}`);
  if (i === -1) return fallback;
  const v = process.argv[i + 1];
  return v && !v.startsWith("--") ? v : true;
}

const env = String(arg("env", "esp32cam"));
const port = arg("port", null);
const loop = !!arg("loop", false);
const labelsPath = resolve(String(arg("labels", "labels.csv")));
const wantHtml = !!arg("html", false) || !!arg("html-only", false);
const htmlOnly = !!arg("html-only", false);
const fixedPin = arg("pin", null);
// Optional D1 ledger mirror: POST each labeled unit to the ops endpoint, in addition to labels.csv.
// `--record <apiBase>` (e.g. https://api.commandagi.com). Best-effort: a failed POST warns but never
// fails the flash — the local CSV is always the source of truth.
const recordBase = (() => {
  const v = arg("record", null);
  return typeof v === "string" ? v.replace(/\/$/, "") : null;
})();

/** Walk up from `start` looking for a `.env`, returning its path or null. */
function findDotenv(start) {
  let dir = start;
  for (let i = 0; i < 8; i++) {
    const p = resolve(dir, ".env");
    if (existsSync(p)) return p;
    const up = dirname(dir);
    if (up === dir) break;
    dir = up;
  }
  return null;
}

/** Read one KEY's value from the repo .env (simple KEY=VALUE, optional quotes). null if absent. */
function envFromDotenv(key) {
  // tools/ → esp32-cam → firmware → repo root; also try cwd, wherever the user ran from.
  const candidates = [findDotenv(dirname(new URL(import.meta.url).pathname)), findDotenv(process.cwd())];
  for (const file of candidates) {
    if (!file) continue;
    const m = readFileSync(file, "utf8").match(new RegExp(`^${key}=(.*)$`, "m"));
    if (m) return m[1].trim().replace(/^["']|["']$/g, "");
  }
  return null;
}

// Service secret for --record: prefer the env, then the repo .env (current API_SERVICE_SECRET, then the
// legacy EDGE_SERVICE_SECRET key — same value, renamed when edge.* → api.* retired).
const recordSecret =
  process.env.API_SERVICE_SECRET ||
  (recordBase ? envFromDotenv("API_SERVICE_SECRET") || envFromDotenv("EDGE_SERVICE_SECRET") || "" : "");

/** A 6-digit PIN (leading zeros allowed — it's a string), uniformly random. */
function mintPin() {
  return String(randomInt(0, 1_000_000)).padStart(6, "0");
}

function run(cmd, args, opts = {}) {
  return new Promise((res, rej) => {
    const p = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], ...opts });
    let out = "";
    p.stdout.on("data", (d) => {
      out += d;
      process.stdout.write(d);
    });
    p.stderr.on("data", (d) => {
      out += d;
      process.stderr.write(d);
    });
    p.on("error", rej);
    p.on("close", (code) => (code === 0 ? res(out) : rej(new Error(`${cmd} exited ${code}`))));
  });
}

/** Serial-port device paths an ESP32 typically enumerates as, sorted. */
function listPorts() {
  try {
    return readdirSync("/dev")
      .filter((n) => /^ttyUSB\d+$/.test(n) || /^ttyACM\d+$/.test(n))
      .map((n) => `/dev/${n}`)
      .sort();
  } catch {
    return [];
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** True if the current user can open `p` for read/write. */
function isWritable(p) {
  try {
    accessSync(p, fsConstants.R_OK | fsConstants.W_OK);
    return true;
  } catch {
    return false;
  }
}

/**
 * Make sure `p` is rw for us. Serial nodes are dialout-owned; if we're not in that group (common on a
 * fresh box, and unavoidable in --loop where the port appears after start), grant it with sudo using
 * SUDO_PASSWORD from the env or the repo .env. Best-effort: warns and continues if there's no password
 * or sudo declines — the flash may still work if you're already in dialout, and fails loudly if not.
 */
async function ensureWritable(p) {
  if (!p || isWritable(p)) return;
  const pw = process.env.SUDO_PASSWORD || envFromDotenv("SUDO_PASSWORD");
  if (!pw) {
    console.warn(
      `⚠ ${p} isn't writable and no SUDO_PASSWORD (env or repo .env) to grant it — ` +
        "continuing, but the flash will fail unless you're in the 'dialout' group " +
        "(sudo usermod -aG dialout $USER, then re-login).",
    );
    return;
  }
  await new Promise((res) => {
    const s = spawn("sudo", ["-S", "-p", "", "chmod", "a+rw", p], { stdio: ["pipe", "ignore", "ignore"] });
    s.on("error", () => res());
    s.on("close", () => res());
    try {
      s.stdin.write(`${pw}\n`);
      s.stdin.end();
    } catch {
      res();
    }
  });
  if (isWritable(p)) console.log(`▶ granted rw on ${p} (sudo via .env)`);
  else console.warn(`⚠ could not grant rw on ${p} — continuing anyway (flash may fail).`);
}

/**
 * Poll until a serial port APPEARS (absent→present edge); resolve its path. Ctrl-C to abort.
 * `known` seeds the set of ports already attached at start so we don't flash one that's just sitting
 * there — but it's maintained by edges: when a known port is unplugged we forget it, so re-plugging a
 * board that re-enumerates under the SAME name (e.g. /dev/ttyUSB0 → unplug → /dev/ttyUSB0) still counts
 * as a fresh board. (Static membership wouldn't — that was the bug.)
 */
async function waitForNewPort(known) {
  const accounted = new Set(known);
  let warned = false;
  for (;;) {
    const now = new Set(listPorts());
    // Forget ports that went away, so a same-name re-plug registers as a new appearance.
    for (const p of [...accounted]) if (!now.has(p)) accounted.delete(p);
    // Any port present now but not yet accounted for is a freshly-plugged board.
    for (const p of now) if (!accounted.has(p)) return p;
    if (!warned) {
      process.stdout.write("⌛ waiting for a board to be plugged in (Ctrl-C to stop) …");
      warned = true;
    } else {
      process.stdout.write(".");
    }
    await sleep(700);
  }
}

/** Poll until `port` disappears (board unplugged). */
async function waitForRemoval(p) {
  let warned = false;
  while (listPorts().includes(p)) {
    if (!warned) {
      process.stdout.write(`⏏ unplug this board (${p}) to flash the next (Ctrl-C to stop) …`);
      warned = true;
    } else {
      process.stdout.write(".");
    }
    await sleep(700);
  }
  if (warned) process.stdout.write("\n");
}

/**
 * Read the BLE advertised-name suffix off the board over serial, returning the 4-hex string (or null).
 * Delegates to tools/read-suffix.py (pyserial): `pio device monitor` can't be used here — miniterm
 * needs an interactive TTY and crashes when scripted, so it never reads anything. The helper resets
 * the board on open so the once-per-boot advertising line reprints within `seconds`.
 */
function readSuffix(seconds = 12, portArg = port) {
  return new Promise((res) => {
    if (!portArg) return res(null); // need a concrete port to open
    const script = resolve("tools/read-suffix.py");
    const p = spawn("python3", [script, String(portArg), String(seconds)], {
      stdio: ["ignore", "pipe", "inherit"], // stderr (boot log) streams through for live debugging
    });
    let out = "";
    p.stdout.on("data", (d) => (out += d));
    p.on("error", () => res(null));
    p.on("close", () => {
      const m = out.trim().match(/([0-9A-Fa-f]{4})/);
      res(m ? m[1].toUpperCase() : null);
    });
  });
}

function renderHtml(rows) {
  const cards = rows
    .map(
      (r) => `
    <div class="label">
      <div class="brand">CommandAGI Cam</div>
      <div class="suffix">${r.suffix || "????"}</div>
      <div class="pin">PIN <b>${r.pin}</b></div>
      <div class="mac">${r.mac || ""}</div>
    </div>`,
    )
    .join("");
  return `<!doctype html><meta charset="utf-8"><title>CommandAGI camera labels</title>
<style>
  body { font: 14px -apple-system, system-ui, sans-serif; margin: 16px; }
  .sheet { display: grid; grid-template-columns: repeat(auto-fill, 180px); gap: 10px; }
  .label { width: 180px; border: 1px solid #111; border-radius: 12px; padding: 12px 14px; }
  .brand { font-size: 11px; color: #666; letter-spacing: .02em; }
  .suffix { font-size: 30px; font-weight: 700; font-family: ui-monospace, monospace; letter-spacing: .06em; }
  .pin { margin-top: 4px; font-size: 15px; }
  .pin b { font-family: ui-monospace, monospace; letter-spacing: .08em; }
  .mac { margin-top: 6px; font-size: 10px; color: #999; font-family: ui-monospace, monospace; }
  @media print { @page { margin: 8mm; } }
</style>
<div class="sheet">${cards}</div>`;
}

function readRows() {
  if (!existsSync(labelsPath)) return [];
  const lines = readFileSync(labelsPath, "utf8").trim().split("\n");
  if (lines.length <= 1) return [];
  return lines.slice(1).map((l) => {
    const [at, mac, suffix, pin, fw] = l.split(",");
    return { at, mac, suffix, pin, fw };
  });
}

/** Mirror one labeled unit into the D1 ledger. Best-effort — warns but never throws. */
async function recordToLedger({ mac, suffix, pin, fw, at }) {
  if (!recordBase) return;
  if (!recordSecret) {
    console.warn(
      "⚠ --record set but no service secret found (env API_SERVICE_SECRET or repo .env " +
        "API_SERVICE_SECRET/EDGE_SERVICE_SECRET) — skipping the D1 ledger mirror.",
    );
    return;
  }
  const flashedBy = `${(() => { try { return userInfo().username; } catch { return "?"; } })()}@${hostname()}`;
  try {
    const res = await fetch(`${recordBase}/ops/cameras/flashed`, {
      method: "POST",
      headers: { "content-type": "application/json", authorization: `Bearer ${recordSecret}` },
      body: JSON.stringify({ mac, suffix: suffix || "", pin, fw, flashedBy, flashedAt: Date.parse(at) }),
    });
    if (res.ok) console.log(`▶ recorded to D1 ledger (${recordBase})`);
    else console.warn(`⚠ D1 ledger POST failed: ${res.status} ${await res.text().catch(() => "")}`);
  } catch (e) {
    console.warn(`⚠ D1 ledger POST errored: ${e.message} (local CSV still written)`);
  }
}

/** Flash + label one board on `portArg` (null = pio auto-detect). Returns the labeled row. */
async function flashOne({ port: portArg, pin }) {
  console.log(`\n▶ Flashing ${portArg || "the connected board"} with PIN ${pin} …\n`);

  // Serial node is dialout-owned — make sure we can open it (sudo-grant via .env if needed).
  await ensureWritable(portArg);

  // Compile the per-board PIN in. PlatformIO honors PLATFORMIO_BUILD_FLAGS and shlex-splits it, which
  // EATS bare double quotes — so `-DCAGI_PROV_PIN="123456"` would reach gcc as the int 123456 and fail
  // (CAGI_PROV_PIN is a const char*). Escape the quotes (\") so a literal "123456" string survives.
  const buildFlags = `-DCAGI_PROV_PIN=\\"${pin}\\"`;
  const uploadArgs = ["run", "-e", env, "-t", "upload"];
  if (portArg) uploadArgs.push("--upload-port", String(portArg));
  await run("pio", uploadArgs, { env: { ...process.env, PLATFORMIO_BUILD_FLAGS: buildFlags } });

  console.log("\n… reading the board back over serial to capture its MAC suffix …");
  const suffix = await readSuffix(9000, portArg);
  if (!suffix) console.warn("⚠ could not read the advertised suffix over serial — recording PIN only.");

  // We can also pull the full MAC from esptool for the ledger (best-effort). --port is an esptool
  // option, so it MUST come AFTER the `--` separator — before it, `pio pkg exec` rejects it.
  let mac = "";
  try {
    const macArgs = ["pkg", "exec", "esptool.py", "--"];
    if (portArg) macArgs.push("--port", String(portArg));
    macArgs.push("read_mac");
    const out = await run("pio", macArgs).catch(() => "");
    const m = out.match(/MAC:\s*([0-9A-Fa-f:]{17})/);
    if (m) mac = m[1].toUpperCase();
  } catch {
    /* esptool not available — fine, suffix is what matters for the label */
  }

  const fw = (readFileSync(resolve("src/config.h"), "utf8").match(/CAGI_FW_VERSION "([^"]+)"/) || [])[1] || "";
  const at = new Date().toISOString();
  if (!existsSync(labelsPath)) {
    mkdirSync(dirname(labelsPath), { recursive: true });
    writeFileSync(labelsPath, "at,mac,suffix,pin,fw\n");
  }
  appendFileSync(labelsPath, `${at},${mac},${suffix || ""},${pin},${fw}\n`);

  if (wantHtml) writeFileSync(labelsPath.replace(/\.csv$/, ".html"), renderHtml(readRows()));

  await recordToLedger({ mac, suffix, pin, fw, at });

  console.log("\n┌──────────────────────────────┐");
  console.log("│  CommandAGI Cam              │");
  console.log(`│  Unit:  ${(suffix || "????").padEnd(20)}│`);
  console.log(`│  PIN:   ${pin.padEnd(20)}│`);
  console.log("└──────────────────────────────┘");
  console.log(`\n✓ labeled → appended to ${labelsPath}.`);
  return { at, mac, suffix, pin, fw };
}

async function main() {
  if (htmlOnly) {
    const rows = readRows();
    const htmlPath = labelsPath.replace(/\.csv$/, ".html");
    writeFileSync(htmlPath, renderHtml(rows));
    console.log(`\nWrote ${rows.length} labels → ${htmlPath}`);
    return;
  }

  if (loop) {
    if (port) console.warn("⚠ --loop ignores --port (it auto-detects each board as you plug it in).");
    if (fixedPin) console.warn("⚠ --loop ignores --pin (each board gets a fresh random PIN).");
    console.log(
      "\n▶ Batch loop — flash one board at a time. Plug a board in (GPIO0→GND for the first flash),\n" +
        "  wait for ✓, unplug it, plug in the next. Ctrl-C to stop.\n",
    );
    let n = 0;
    // Boards already present at start are "known" — we only flash freshly-plugged ones, so we don't
    // re-flash a board that's been sitting attached.
    let known = listPorts();
    if (known.length) {
      console.log(`(ignoring ${known.length} port(s) already attached: ${known.join(", ")} — unplug & replug to flash)`);
    }
    for (;;) {
      const fresh = await waitForNewPort(known);
      process.stdout.write("\n");
      console.log(`● detected ${fresh}`);
      // pio's USB reset can take a beat to settle after enumeration.
      await sleep(800);
      try {
        await flashOne({ port: fresh, pin: mintPin() });
        n++;
        console.log(`\n———  ${n} board(s) flashed this run  ———`);
      } catch (e) {
        console.error(`✗ ${fresh} failed: ${e.message} — unplug it and try the next board.`);
      }
      await waitForRemoval(fresh);
      // Re-baseline so the just-removed port (and any unrelated ones) are accounted for.
      known = listPorts();
    }
  }

  // Single shot.
  await flashOne({ port, pin: String(fixedPin || mintPin()) });
  console.log("Unplug this board and plug in the next (or use --loop to stay running).\n");
}

main().catch((e) => {
  console.error(`\n✗ ${e.message}`);
  process.exit(1);
});
