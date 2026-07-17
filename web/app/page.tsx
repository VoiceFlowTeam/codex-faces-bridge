"use client";

import type { CSSProperties, KeyboardEvent, PointerEvent, WheelEvent } from "react";
import { useCallback, useEffect, useRef, useState } from "react";
import {
  Badge,
  Check,
  GitBranch,
  Mic,
  Plus,
  X,
  Zap,
} from "lucide-react";

const THREAD_COLORS = [
  "#8a98d8",
  "#ef6d91",
  "#8d78e8",
  "#ff9b72",
  "#64bfc1",
  "#d3ad5a",
];

type DialProps = {
  label: string;
  value: number;
  onChange: (value: number) => void;
  variant: "light" | "dark";
};

function RotaryDial({ label, value, onChange, variant }: DialProps) {
  const updateFromPointer = (event: PointerEvent<HTMLDivElement>) => {
    const bounds = event.currentTarget.getBoundingClientRect();
    const x = event.clientX - (bounds.left + bounds.width / 2);
    const y = event.clientY - (bounds.top + bounds.height / 2);
    let angle = (Math.atan2(y, x) * 180) / Math.PI + 90;
    if (angle > 180) angle -= 360;
    const clamped = Math.max(-135, Math.min(135, angle));
    onChange(Math.round(((clamped + 135) / 270) * 100));
  };

  const handlePointerDown = (event: PointerEvent<HTMLDivElement>) => {
    event.currentTarget.setPointerCapture(event.pointerId);
    updateFromPointer(event);
  };

  const handlePointerMove = (event: PointerEvent<HTMLDivElement>) => {
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      updateFromPointer(event);
    }
  };

  const handleWheel = (event: WheelEvent<HTMLDivElement>) => {
    event.preventDefault();
    onChange(Math.max(0, Math.min(100, value + (event.deltaY < 0 ? 4 : -4))));
  };

  const handleKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    if (["ArrowUp", "ArrowRight"].includes(event.key)) {
      event.preventDefault();
      onChange(Math.min(100, value + 4));
    } else if (["ArrowDown", "ArrowLeft"].includes(event.key)) {
      event.preventDefault();
      onChange(Math.max(0, value - 4));
    } else if (event.key === "Home") {
      onChange(0);
    } else if (event.key === "End") {
      onChange(100);
    }
  };

  const style = { "--dial-angle": `${-135 + value * 2.7}deg` } as CSSProperties;

  return (
    <div
      className={`dial-shell dial-${variant}`}
      role="slider"
      tabIndex={0}
      aria-label={label}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={value}
      onPointerDown={handlePointerDown}
      onPointerMove={handlePointerMove}
      onWheel={handleWheel}
      onKeyDown={handleKeyDown}
    >
      <div className="dial-face" style={style}>
        <span className="dial-tick" />
      </div>
    </div>
  );
}

export default function Home() {
  const [threads, setThreads] = useState(() => Array(6).fill(false) as boolean[]);
  const [recording, setRecording] = useState(false);
  const [connected, setConnected] = useState(true);
  const [agentMode, setAgentMode] = useState(false);
  const [modeDial, setModeDial] = useState(50);
  const [reasoning, setReasoning] = useState(58);
  const [lastEvent, setLastEvent] = useState("Ready — interact with the controls");
  const [flashingAction, setFlashingAction] = useState<string | null>(null);
  const eventTimer = useRef<number | null>(null);

  const announce = useCallback((message: string) => {
    setLastEvent(message);
    if (eventTimer.current !== null) window.clearTimeout(eventTimer.current);
    eventTimer.current = window.setTimeout(() => {
      setLastEvent("Ready — interact with the controls");
    }, 2600);
  }, []);

  useEffect(() => {
    return () => {
      if (eventTimer.current !== null) window.clearTimeout(eventTimer.current);
    };
  }, []);

  const toggleThread = useCallback(
    (index: number) => {
      setThreads((current) => {
        const next = [...current];
        next[index] = !next[index];
        announce(`Thread ${index + 1} ${next[index] ? "activated" : "cleared"}`);
        return next;
      });
    },
    [announce],
  );

  const toggleRecording = useCallback(() => {
    setRecording((current) => {
      announce(current ? "Voice capture stopped" : "Listening…");
      return !current;
    });
  }, [announce]);

  const triggerAction = (id: string, message: string) => {
    setFlashingAction(id);
    announce(message);
    window.setTimeout(() => setFlashingAction(null), 340);
  };

  useEffect(() => {
    const handleShortcut = (event: globalThis.KeyboardEvent) => {
      const target = event.target as HTMLElement | null;
      if (target?.matches("input, textarea, select, [contenteditable='true']")) return;
      if (/^[1-6]$/.test(event.key)) toggleThread(Number(event.key) - 1);
      if (event.key.toLowerCase() === "r") toggleRecording();
    };
    window.addEventListener("keydown", handleShortcut);
    return () => window.removeEventListener("keydown", handleShortcut);
  }, [toggleRecording, toggleThread]);

  const updateModeDial = (value: number) => {
    setModeDial(value);
    const modes = ["Focus", "Build", "Review", "Ship"];
    const mode = modes[Math.min(3, Math.round((value / 100) * 3))];
    announce(`Mode: ${mode}`);
  };

  const updateReasoning = (value: number) => {
    setReasoning(value);
    announce(`Reasoning effort: ${value}%`);
  };

  return (
    <main className="simulator-page">
      <section className="device-wrap" aria-label="Codex Micro interactive simulator">
        <div className={`device ${recording ? "is-recording" : ""} ${agentMode ? "is-agent" : ""}`}>
          <div className="device-rim" />
          <div className="faceplate">
            <span className="screw screw-tl" aria-hidden="true" />
            <span className="screw screw-tr" aria-hidden="true" />
            <span className="screw screw-bl" aria-hidden="true" />
            <span className="screw screw-br" aria-hidden="true" />

            <span className="side-copy side-copy-left">Work Louder&nbsp; | &nbsp;OpenAI 2026</span>
            <span className="side-copy side-copy-right">You can just build things</span>
            <span className="top-arrow" aria-hidden="true">↑</span>
            <span className="bottom-copy">Let&apos;s build</span>

            <div className="control-grid">
              <RotaryDial label="Mode selector" value={modeDial} onChange={updateModeDial} variant="light" />

              {threads.slice(0, 2).map((active, index) => (
                <button
                  className="thread-key"
                  data-active={active}
                  key={index}
                  style={{ "--thread-color": THREAD_COLORS[index] } as CSSProperties}
                  onClick={() => toggleThread(index)}
                  aria-label={`Toggle thread ${index + 1}`}
                  aria-pressed={active}
                >
                  <span className="thread-light"><Plus strokeWidth={2.4} /></span>
                </button>
              ))}

              <RotaryDial label="Reasoning effort" value={reasoning} onChange={updateReasoning} variant="dark" />

              {threads.slice(2).map((active, offset) => {
                const index = offset + 2;
                return (
                  <button
                    className="thread-key"
                    data-active={active}
                    key={index}
                    style={{ "--thread-color": THREAD_COLORS[index] } as CSSProperties}
                    onClick={() => toggleThread(index)}
                    aria-label={`Toggle thread ${index + 1}`}
                    aria-pressed={active}
                  >
                    <span className="thread-light"><Plus strokeWidth={2.4} /></span>
                  </button>
                );
              })}

              <button
                className="action-key"
                data-flash={flashingAction === "fast"}
                onClick={() => triggerAction("fast", "Fast mode toggled")}
                aria-label="Toggle fast mode"
              ><Zap /></button>
              <button
                className="action-key"
                data-flash={flashingAction === "approve"}
                onClick={() => triggerAction("approve", "Approved")}
                aria-label="Approve"
              ><span className="circle-icon"><Check /></span></button>
              <button
                className="action-key"
                data-flash={flashingAction === "decline"}
                onClick={() => triggerAction("decline", "Declined")}
                aria-label="Decline"
              ><span className="circle-icon"><X /></span></button>
              <button
                className="action-key"
                data-flash={flashingAction === "branch"}
                onClick={() => triggerAction("branch", "New branch created")}
                aria-label="Create branch"
              ><GitBranch /></button>

              <div className="power-cluster">
                <div className="battery-bars" aria-label="Battery 67 percent">
                  <span />
                  <span className="filled" />
                  <span className="filled" />
                </div>
                <button
                  className="power-button"
                  data-connected={connected}
                  onClick={() => {
                    setConnected((current) => !current);
                    announce(connected ? "Device disconnected" : "Device connected");
                  }}
                  aria-label="Toggle device connection"
                  aria-pressed={connected}
                />
              </div>

              <button
                className="mic-key"
                data-recording={recording}
                onClick={toggleRecording}
                aria-label={recording ? "Stop recording" : "Start recording"}
                aria-pressed={recording}
              >
                <span className="mic-glyph" aria-hidden="true"><Mic /></span>
                <span className="sound-waves" aria-hidden="true"><i /><i /><i /><i /></span>
              </button>

              <button
                className="action-key assistant-key"
                data-active={agentMode}
                onClick={() => {
                  setAgentMode((current) => !current);
                  announce(agentMode ? "Agent layer off" : "Agent layer on");
                }}
                aria-label="Toggle agent layer"
                aria-pressed={agentMode}
              >
                <span className="agent-glyph" aria-hidden="true">
                  <Badge />
                  <span>&gt;_</span>
                </span>
              </button>
            </div>
          </div>
        </div>

        <div className="status-console" aria-live="polite">
          <span className={`connection-dot ${connected ? "online" : ""}`} />
          <span>{lastEvent}</span>
          <span className="shortcut-hint">1–6 threads · R voice · drag or scroll dials</span>
        </div>
      </section>
    </main>
  );
}
