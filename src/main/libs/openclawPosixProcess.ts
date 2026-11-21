// POSIX (/proc for Linux, `ps` for macOS) counterpart of openclawWindowsProcess.
// Same identity shape so openclawLockRecovery can classify lock owners on every
// desktop platform. Termination is two-phase (SIGTERM, then SIGKILL) with
// liveness confirmed through the same identity source used for inspection.
import { execFile } from 'node:child_process';
import fs from 'node:fs';

export type PosixProcessStatus = 'running' | 'gone' | 'stopped' | 'unknown';
export interface PosixProcessIdentity {
  status: PosixProcessStatus;
  pid: number;
  /** Linux: raw /proc starttime in clock ticks (matches the gateway lock payload). */
  startTime?: number;
  /** Epoch ms of process creation (best effort; seconds precision via ps on macOS). */
  creationTime?: string;
  executablePath?: string;
  args?: string[];
  parentPid?: number;
  parentAlive?: boolean;
  reason?: string;
}

const STOP_POLL_MS = 250;
const STOP_TERM_TIMEOUT_MS = 6_000;
const STOP_KILL_TIMEOUT_MS = 3_000;
const PS_TIMEOUT_MS = 10_000;

function psRun(pid: number, format: string): Promise<string | null> {
  return new Promise(resolve => {
    execFile('/bin/ps', ['-p', String(pid), '-o', format], { timeout: PS_TIMEOUT_MS, encoding: 'utf8' },
      (error, stdout) => {
        // ps exits 1 when the pid does not exist.
        if (error || !stdout) { resolve(null); return; }
        resolve(stdout);
      });
  });
}

function statFields(pid: number): { ppid: number; starttime: number } | null {
  let raw: string;
  try { raw = fs.readFileSync(`/proc/${pid}/stat`, 'utf8'); } catch { return null; }
  // comm may contain spaces and parentheses; everything after the last ')' is fixed-order.
  const close = raw.lastIndexOf(')');
  if (close < 0) return null;
  const rest = raw.slice(close + 2).split(' ');
  // rest[0] is field 3 (state); fields are 1-based in proc(5).
  const ppid = Number(rest[1]);
  const starttime = Number(rest[19]);
  if (!Number.isFinite(ppid) || !Number.isFinite(starttime)) return null;
  return { ppid, starttime };
}

function procArgs(pid: number): string[] | undefined {
  try {
    const raw = fs.readFileSync(`/proc/${pid}/cmdline`, 'utf8');
    const args = raw.split('\0').filter(value => value.length > 0);
    return args.length > 0 ? args : undefined;
  } catch { return undefined; }
}

function procExe(pid: number): string | undefined {
  try { return fs.readlinkSync(`/proc/${pid}/exe`); } catch { return undefined; }
}

function isReaperParent(pid: number): boolean {
  // Orphans are reparented to init (1), a user systemd instance, or (in
  // containers) whatever reaper owns the namespace. Those parents did not
  // spawn the lock owner, so treat them exactly like a dead parent.
  if (pid === 1) return false;
  try {
    const comm = fs.readFileSync(`/proc/${pid}/comm`, 'utf8').trim();
    return comm !== 'systemd' && comm !== 'init' && comm !== 'tini' && comm !== 'dumb-init';
  } catch { return false; }
}

function inspectLinuxProcess(pid: number): PosixProcessIdentity {
  const stat = statFields(pid);
  if (!stat) return { pid, status: 'gone' };
  const identity: PosixProcessIdentity = {
    pid, status: 'running', startTime: stat.starttime, parentPid: stat.ppid,
    // Reparented to init/systemd/reaper = the original parent is gone.
    parentAlive: !isReaperParent(stat.ppid),
    executablePath: procExe(pid), args: procArgs(pid),
  };
  try {
    const bootMs = Number(/btime (\d+)/.exec(fs.readFileSync('/proc/stat', 'utf8'))?.[1]) * 1000;
    if (Number.isFinite(bootMs)) identity.creationTime = String(bootMs + Math.floor(stat.starttime / 100));
  } catch { /* creationTime stays undefined; classification only needs it on the stop path */ }
  return identity;
}

interface PsSnapshot { ppid: number; lstart: string; command: string; }

async function psSnapshot(pid: number): Promise<PsSnapshot | null> {
  const out = await psRun(pid, 'pid=,ppid=,lstart=,command=');
  if (!out) return null;
  const line = out.split('\n').find(value => value.trim().length > 0);
  if (!line) return null;
  // Format: "  PID  PPID                   LSTART COMMAND" — match by the pid column.
  const match = new RegExp(`^\\s*${pid}\\s+(\\d+)\\s+(\\w{3} \\w{3}\\s+\\d+ \\d\\d:\\d\\d:\\d\\d \\d{4}) (.+)$`).exec(line);
  if (!match) return null;
  return { ppid: Number(match[1]), lstart: match[2], command: match[3] };
}

function reaperParentMacOS(ppid: number): boolean {
  return ppid === 1;
}

async function inspectMacOSProcess(pid: number): Promise<PosixProcessIdentity> {
  const snapshot = await psSnapshot(pid);
  if (!snapshot) return { pid, status: 'gone' };
  const creationMs = Date.parse(snapshot.lstart);
  const identity: PosixProcessIdentity = {
    pid, status: 'running', parentPid: snapshot.ppid,
    parentAlive: reaperParentMacOS(snapshot.ppid) ? false : true,
    args: snapshot.command.split(' ').filter(value => value.length > 0),
  };
  if (Number.isFinite(creationMs)) {
    identity.startTime = creationMs;
    identity.creationTime = String(creationMs);
  }
  return identity;
}

export function inspectPosixProcess(pid: number): Promise<PosixProcessIdentity> {
  if (!Number.isSafeInteger(pid) || pid <= 0) {
    return Promise.resolve({ pid, status: 'unknown', reason: 'Unsupported process query.' });
  }
  if (process.platform === 'linux') return Promise.resolve(inspectLinuxProcess(pid));
  if (process.platform === 'darwin') return inspectMacOSProcess(pid);
  return Promise.resolve({ pid, status: 'unknown', reason: 'Unsupported process query.' });
}

async function waitUntilGone(pid: number, deadlineAt: number): Promise<boolean> {
  while (Date.now() < deadlineAt) {
    if ((await inspectPosixProcess(pid)).status === 'gone') return true;
    await new Promise(resolve => setTimeout(resolve, STOP_POLL_MS));
  }
  return (await inspectPosixProcess(pid)).status === 'gone';
}

function signal(pid: number, signal: NodeJS.Signals): boolean {
  try { process.kill(pid, signal); return true; } catch { return false; }
}

export async function stopVerifiedPosixProcess(identity: PosixProcessIdentity): Promise<PosixProcessIdentity> {
  if (identity.status !== 'running') {
    throw new Error('Cannot stop a process without a verified running identity.');
  }
  const before = await inspectPosixProcess(identity.pid);
  // Identity must be unchanged between classification and termination, or the
  // pid may have been reused by an unrelated process.
  if (before.status !== 'running' || before.startTime !== identity.startTime) {
    return { pid: identity.pid, status: 'unknown', reason: 'Process identity changed before termination.' };
  }
  if (!signal(identity.pid, 'SIGTERM')) return { pid: identity.pid, status: 'gone' };
  if (await waitUntilGone(identity.pid, Date.now() + STOP_TERM_TIMEOUT_MS)) {
    return { pid: identity.pid, status: 'stopped' };
  }
  if (!signal(identity.pid, 'SIGKILL')) return { pid: identity.pid, status: 'gone' };
  if (await waitUntilGone(identity.pid, Date.now() + STOP_KILL_TIMEOUT_MS)) {
    return { pid: identity.pid, status: 'stopped' };
  }
  return { pid: identity.pid, status: 'unknown', reason: 'Process exit was not confirmed.' };
}
