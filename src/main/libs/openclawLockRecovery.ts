// Manual repair policy. Native OpenClaw still owns lock acquisition/reclamation.
import fs from 'node:fs';
import path from 'node:path';

import { type OpenClawCompatibilityRepairReport, OpenClawRepairPhase } from '../../shared/openclawEngine/repair';
import { assertOwnedRepairPath } from './openclawCompatibilityRepairCore';
import { type GatewayLockPayload, parseGatewayLockPayload, resolveGatewayLockDir, resolveGatewayLockPathForConfig } from './openclawGatewayLock';
import { inspectPosixProcess, stopVerifiedPosixProcess, type PosixProcessIdentity } from './openclawPosixProcess';
import { inspectWindowsProcess, stopVerifiedWindowsProcess, type WindowsProcessIdentity, WindowsProcessStatus } from './openclawWindowsProcess';

export const LockOwnerDecision = {
  Dead: 'dead', Reused: 'reused', Unrelated: 'unrelated', Orphan: 'orphan', Active: 'active', Unknown: 'unknown',
} as const;
export type LockOwnerDecision = typeof LockOwnerDecision[keyof typeof LockOwnerDecision];
export const NativeLockOwnerStatus = { Alive: 'alive', Dead: 'dead', Unknown: 'unknown' } as const;
type NativeLockOwnerStatus = typeof NativeLockOwnerStatus[keyof typeof NativeLockOwnerStatus];
const LockRole = { Gateway: 'gateway', Maintenance: 'sqlite-maintenance' } as const;
const ORPHAN_GRACE_MS = 30_000;
const CLOCK_TOLERANCE_MS = 2_000;
type UnifiedProcessIdentity = WindowsProcessIdentity | PosixProcessIdentity;

export interface LockRecoveryOptions {
  stateDir: string; configPath: string; runtimeRoot: string; executablePath: string; backupDir: string; env: NodeJS.ProcessEnv;
  platform?: NodeJS.Platform;
}
interface RecoveryDependencies {
  acquireLock: (options: {
    env: NodeJS.ProcessEnv; role: 'sqlite-maintenance'; timeoutMs: number;
    inspectOwner?: (payload: GatewayLockPayload) => Promise<NativeLockOwnerStatus>;
  }) => Promise<{ release: () => Promise<void> } | null>;
  inspect?: typeof inspectWindowsProcess;
  stop?: typeof stopVerifiedWindowsProcess;
  healthy?: (port: number) => Promise<boolean>;
  platform?: NodeJS.Platform;
  now?: () => number;
}

function samePath(left: string | undefined, right: string): boolean {
  const canonical = (value: string) => {
    try { return fs.realpathSync.native(value).toLowerCase(); } catch { return path.resolve(value).toLowerCase(); }
  };
  return Boolean(left) && canonical(left!) === canonical(right);
}

export function classifyLockOwner(
  payload: GatewayLockPayload, identity: UnifiedProcessIdentity, options: LockRecoveryOptions, now = Date.now(),
): LockOwnerDecision {
  // Linux lock payloads carry /proc starttime in clock ticks (boot-relative,
  // ~1e6 for realistic uptimes); Windows and macOS carry epoch milliseconds
  // (>1e11). Detect the unit by magnitude instead of trusting the host
  // platform — unit tests run anywhere but use realistic epoch-like values.
  const identityStartTimeIsEpoch = identity.startTime !== undefined && identity.startTime > 100_000_000_000;
  if (identity.status === WindowsProcessStatus.Gone) return LockOwnerDecision.Dead;
  if (identity.status !== WindowsProcessStatus.Running || identity.pid !== payload.pid || identity.startTime === undefined) return LockOwnerDecision.Unknown;
  if (payload.startTime !== undefined && identity.startTime !== undefined) {
    const drift = Math.abs(identity.startTime - payload.startTime);
    const sameUnit = identityStartTimeIsEpoch === (payload.startTime > 100_000_000_000);
    const tolerance = sameUnit && !identityStartTimeIsEpoch ? 0 : CLOCK_TOLERANCE_MS;
    if (drift > tolerance || !sameUnit) return LockOwnerDecision.Reused;
  }
  const createdAt = Date.parse(payload.createdAt ?? '');
  if (payload.startTime === undefined && identityStartTimeIsEpoch && Number.isFinite(createdAt)
    && identity.startTime > createdAt + CLOCK_TOLERANCE_MS) return LockOwnerDecision.Reused;
  const role = payload.role ?? LockRole.Gateway;
  if (role !== LockRole.Gateway && role !== LockRole.Maintenance) return LockOwnerDecision.Active;
  if (!identity.executablePath) return LockOwnerDecision.Unknown;
  const basename = path.basename(identity.executablePath).toLowerCase();
  // A native OS executable cannot host an Electron/Node OpenClaw writer. Keep
  // unfamiliar application hosts conservative; readable argv provides more
  // evidence. Only meaningful when the identity path really sits in the
  // Windows system root — POSIX paths never do, so the branch no-ops there.
  const systemRoot = process.env.SystemRoot ?? 'C:\\Windows';
  const systemRelative = path.relative(path.resolve(systemRoot), path.resolve(identity.executablePath));
  const inSystemRoot = systemRelative && !systemRelative.startsWith('..') && !path.isAbsolute(systemRelative);
  if (inSystemRoot && !/^(?:node|electron|lobsterai|openclaw)(?:\.exe)?$/i.test(basename)) return LockOwnerDecision.Unrelated;
  if (!identity.args?.length) return LockOwnerDecision.Unknown;
  const entries = (role === LockRole.Gateway ? ['gateway-launcher.cjs', 'openclaw.mjs']
    : ['openclaw-startup-state-migration.mjs', 'openclaw-startup-compat.mjs', 'openclaw.mjs'])
    .map(entry => path.join(options.runtimeRoot, entry));
  const entry = entries.find(candidate => identity.args!.some(arg => samePath(arg, candidate)));
  if (!entry) {
    // External OpenClaw/Node runtimes may legitimately share this state. Only
    // positively unrelated executables are reclaimable without a startTime.
    if (/^(?:node|electron|lobsterai|openclaw)(?:\.exe)?$/i.test(basename)
      || identity.args.some(arg => /openclaw|gateway/i.test(arg))) return LockOwnerDecision.Active;
    return LockOwnerDecision.Unrelated;
  }
  if (!samePath(identity.executablePath, options.executablePath)
    || !samePath(payload.stateDir, options.stateDir) || !samePath(payload.configPath, options.configPath)
    || payload.startTime === undefined || !identity.creationTime || !Number.isFinite(createdAt)
    || createdAt > now || now - createdAt < ORPHAN_GRACE_MS || identity.parentAlive !== false) return LockOwnerDecision.Active;
  if (path.basename(entry) === 'openclaw.mjs') {
    const index = identity.args.findIndex(arg => samePath(arg, entry));
    if (identity.args[index + 1] !== (role === LockRole.Gateway ? LockRole.Gateway : 'doctor')) return LockOwnerDecision.Active;
  }
  if (role === LockRole.Gateway && payload.port === undefined) return LockOwnerDecision.Unknown;
  return LockOwnerDecision.Orphan;
}

async function isHealthy(port: number): Promise<boolean> {
  try { return (await fetch(`http://127.0.0.1:${port}/healthz`, { signal: AbortSignal.timeout(2_000) })).ok; }
  catch { return false; }
}

export async function recoverOpenClawLockOwners(
  options: LockRecoveryOptions, dependencies: RecoveryDependencies,
): Promise<OpenClawCompatibilityRepairReport> {
  const report: OpenClawCompatibilityRepairReport = {
    phase: OpenClawRepairPhase.LockRecovery, success: false, changes: [], backups: [],
  };
  const diagnostics: object[] = [];
  const inspect = dependencies.inspect ?? inspectWindowsProcess;
  const now = dependencies.now ?? Date.now;
  const lockDir = resolveGatewayLockDir(options.stateDir);
  const paths = [path.join(lockDir, 'gateway.state.lock'), resolveGatewayLockPathForConfig(options.configPath, lockDir)];
  const diagnosticPath = path.join(options.backupDir, 'lock-owner-diagnostics.json');
  const save = () => fs.writeFileSync(diagnosticPath, JSON.stringify(diagnostics, null, 2), { mode: 0o600 });
  const observe = async (payload: GatewayLockPayload) => {
    const identity = await inspect(payload.pid);
    const decision = classifyLockOwner(payload, identity, options, now());
    // Do not persist argv: it may contain a gateway token or provider credential.
    const { args: _args, ...metadata } = identity;
    diagnostics.push({ observedAt: new Date(now()).toISOString(), payload, process: metadata, decision });
    save();
    return { identity, decision };
  };
  try {
    // Native lock resolution canonicalizes redirected/junction state roots.
    // Validate against that same root; aliases below it remain forbidden.
    const ownedStateDir = fs.realpathSync.native(options.stateDir);
    const snapshots = paths.map(lockPath => {
      assertOwnedRepairPath(ownedStateDir, lockPath);
      let raw: string | undefined;
      try {
        if (fs.statSync(lockPath).size > 64 * 1024) throw new Error(`Unexpectedly large gateway lock: ${lockPath}`);
        raw = fs.readFileSync(lockPath, 'utf8');
      } catch (error) { if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error; }
      return { lockPath, raw, payload: raw === undefined ? null : parseGatewayLockPayload(raw) };
    });
    diagnostics.push({ locks: snapshots.map(({ lockPath, raw }) => ({ lockPath, raw })) });
    save();
    report.backups.push(diagnosticPath);
    const windows = (dependencies.platform ?? process.platform) === 'win32';
    if (windows) {
      const plans = [];
      for (const snapshot of snapshots) {
        if (snapshot.payload) plans.push({ ...snapshot, ...await observe(snapshot.payload) });
      }
      const blocked = plans.find(plan => plan.decision === LockOwnerDecision.Active || plan.decision === LockOwnerDecision.Unknown);
      if (blocked) throw new Error(`Lock owner PID ${blocked.payload!.pid} is ${blocked.decision}; ownership recovery was not authorized. See ${diagnosticPath}`);
      const stopped = new Set<number>();
      for (const plan of plans) {
        if (plan.decision !== LockOwnerDecision.Orphan || stopped.has(plan.identity.pid)) continue;
        // The gateway has an independent liveness signal. Two failed probes are
        // required; a healthy detached gateway remains protected.
        if (plan.payload!.port !== undefined) {
          const healthy = dependencies.healthy ?? isHealthy;
          if (await healthy(plan.payload!.port) || await healthy(plan.payload!.port)) throw new Error(`Gateway PID ${plan.identity.pid} is healthy; stop its owning instance before repair.`);
        }
        for (const snapshot of snapshots) {
          const current = fs.existsSync(snapshot.lockPath) ? fs.readFileSync(snapshot.lockPath, 'utf8') : undefined;
          if (current !== snapshot.raw) throw new Error('Gateway lock changed before process termination. Retry repair.');
        }
        const fresh = await observe(plan.payload!);
        if (fresh.decision !== LockOwnerDecision.Orphan || fresh.identity.creationTime !== plan.identity.creationTime) throw new Error('Lock owner identity changed before process termination.');
        if (fs.readFileSync(plan.lockPath, 'utf8') !== plan.raw) throw new Error('Gateway lock changed during process verification. Retry repair.');
        const result = await (dependencies.stop ?? stopVerifiedWindowsProcess)(fresh.identity);
        diagnostics.push({ termination: result }); save();
        if (result.status !== WindowsProcessStatus.Stopped && result.status !== WindowsProcessStatus.Gone) throw new Error(`Could not confirm lock owner PID ${plan.identity.pid} exited: ${result.reason ?? result.status}`);
        stopped.add(plan.identity.pid);
        report.changes.push(`Stopped verified orphan OpenClaw process ${plan.identity.pid}.`);
      }
    }
    // This invokes native lifecycle + per-lock SQLite coordinators and the
    // native file manager's second ownership check / remove-if-unchanged path.
    const lease = await dependencies.acquireLock({ env: options.env, role: LockRole.Maintenance, timeoutMs: 2_000,
      ...(windows ? { inspectOwner: async (payload: GatewayLockPayload): Promise<NativeLockOwnerStatus> => {
        const { decision } = await observe(payload);
        if ([LockOwnerDecision.Dead, LockOwnerDecision.Reused, LockOwnerDecision.Unrelated].some(value => value === decision)) return NativeLockOwnerStatus.Dead;
        return decision === LockOwnerDecision.Unknown ? NativeLockOwnerStatus.Unknown : NativeLockOwnerStatus.Alive;
      } } : {}),
    });
    if (!lease) throw new Error('Native gateway lease was not acquired.');
    await lease.release();
    for (const snapshot of snapshots) if (snapshot.raw !== undefined) report.changes.push(`Reclaimed stale gateway lock ${path.basename(snapshot.lockPath)}.`);
    report.success = true;
  } catch (error) {
    report.error = error instanceof Error ? error.message : String(error);
    diagnostics.push({ failure: report.error }); save();
  }
  fs.writeFileSync(path.join(options.backupDir, 'lock-recovery-report.json'), JSON.stringify(report, null, 2), { mode: 0o600 });
  return report;
}

export interface GatewayLockReclaimResult {
  stoppedPids: number[];
  blockedReason?: string;
  diagnosticsPath?: string;
}
export interface GatewayLockReclaimDependencies {
  inspect?: (pid: number) => Promise<UnifiedProcessIdentity>;
  stop?: (identity: UnifiedProcessIdentity) => Promise<UnifiedProcessIdentity>;
  healthy?: (port: number) => Promise<boolean>;
  now?: () => number;
}

/**
 * Stop gateway lock owners that no live LobsterAI instance owns anymore —
 * processes whose parent died and were reaped by init/systemd/launchd (crash
 * leftovers). The native repair helper refuses to touch locks held by live
 * owners, so this must run before it; without it, a crashed instance's
 * orphaned gateway blocks Quick Repair on Linux/macOS forever.
 *
 * Conservative on purpose: live parents, healthy detached gateways, unknown
 * identities, and young locks (grace period) all block the reclaim.
 */
export async function reclaimOrphanedGatewayStateLocks(
  options: LockRecoveryOptions,
  dependencies: GatewayLockReclaimDependencies = {},
): Promise<GatewayLockReclaimResult> {
  const platform = options.platform ?? process.platform;
  const result: GatewayLockReclaimResult = { stoppedPids: [] };
  if (platform !== 'win32' && platform !== 'linux' && platform !== 'darwin') {
    return { ...result, blockedReason: `Unsupported platform: ${platform}.` };
  }
  const inspect = dependencies.inspect
    ?? (platform === 'win32' ? inspectWindowsProcess : inspectPosixProcess);
  const stop = dependencies.stop
    ?? (platform === 'win32'
      ? async (identity: UnifiedProcessIdentity) => stopVerifiedWindowsProcess(identity as WindowsProcessIdentity)
      : async (identity: UnifiedProcessIdentity) => stopVerifiedPosixProcess(identity as PosixProcessIdentity));
  const now = dependencies.now ?? Date.now;
  const lockDir = resolveGatewayLockDir(options.stateDir);
  const paths = [path.join(lockDir, 'gateway.state.lock'), resolveGatewayLockPathForConfig(options.configPath, lockDir)];
  const diagnosticPath = path.join(options.backupDir, 'lock-owner-diagnostics.json');
  const diagnostics: object[] = [];
  const save = () => {
    try { fs.writeFileSync(diagnosticPath, JSON.stringify(diagnostics, null, 2), { mode: 0o600 }); } catch { /* best effort */ }
  };
  result.diagnosticsPath = diagnosticPath;
  try {
    const ownedStateDir = fs.realpathSync.native(options.stateDir);
    const snapshots = paths.map(lockPath => {
      assertOwnedRepairPath(ownedStateDir, lockPath);
      let raw: string | undefined;
      try {
        if (fs.statSync(lockPath).size > 64 * 1024) throw new Error(`Unexpectedly large gateway lock: ${lockPath}`);
        raw = fs.readFileSync(lockPath, 'utf8');
      } catch (error) { if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error; }
      return { lockPath, raw, payload: raw === undefined ? null : parseGatewayLockPayload(raw) };
    });
    diagnostics.push({ locks: snapshots.map(({ lockPath, raw }) => ({ lockPath, raw })) });

    const plans = [];
    for (const snapshot of snapshots) {
      if (!snapshot.payload) continue;
      const identity = await inspect(snapshot.payload.pid);
      const decision = classifyLockOwner(snapshot.payload, identity, options, now());
      const { args: _args, ...metadata } = identity;
      diagnostics.push({ observedAt: new Date(now()).toISOString(), payload: snapshot.payload, process: metadata, decision });
      plans.push({ ...snapshot, identity, decision });
    }
    save();

    const blocked = plans.find(plan => plan.decision === LockOwnerDecision.Active || plan.decision === LockOwnerDecision.Unknown);
    if (blocked) {
      result.blockedReason = `Lock owner PID ${blocked.payload!.pid} is ${blocked.decision}; not safe to reclaim.`;
      return result;
    }
    const healthy = dependencies.healthy ?? isHealthy;
    for (const plan of plans) {
      if (plan.decision !== LockOwnerDecision.Orphan) continue;
      // The gateway has an independent liveness signal. Two failed probes are
      // required; a healthy detached gateway remains protected.
      if (plan.payload!.port !== undefined) {
        if (await healthy(plan.payload!.port) || await healthy(plan.payload!.port)) {
          result.blockedReason = `Gateway PID ${plan.identity.pid} is still healthy; its owning instance should stop it.`;
          return result;
        }
      }
      for (const snapshot of snapshots) {
        const current = fs.existsSync(snapshot.lockPath) ? fs.readFileSync(snapshot.lockPath, 'utf8') : undefined;
        if (current !== snapshot.raw) throw new Error('Gateway lock changed before process termination. Retry repair.');
      }
      const fresh = await inspect(plan.identity.pid);
      const freshDecision = classifyLockOwner(plan.payload!, fresh, options, now());
      if (freshDecision !== LockOwnerDecision.Orphan || fresh.startTime !== plan.identity.startTime) {
        throw new Error('Lock owner identity changed before process termination.');
      }
      const termination = await stop(fresh);
      diagnostics.push({ termination }); save();
      if (termination.status !== 'stopped' && termination.status !== 'gone') {
        throw new Error(`Could not confirm lock owner PID ${plan.identity.pid} exited: ${termination.reason ?? termination.status}`);
      }
      result.stoppedPids.push(plan.identity.pid);
    }
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    result.blockedReason = message;
    diagnostics.push({ failure: message }); save();
  }
  return result;
}
