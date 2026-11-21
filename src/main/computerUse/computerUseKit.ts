import { app, systemPreferences } from 'electron';
import fs from 'fs';
import path from 'path';

import {
  ComputerUseKitBundle,
  ComputerUseKitBundleIntegrity,
  ComputerUseKitId,
  ComputerUseKitMetadata,
  ComputerUseSkillId,
} from '../../shared/computerUse/constants';
import {
  type InstalledKitRecord,
  type InstalledKitSkills,
  type KitSkillMetadata,
  KitStoreKey,
} from '../../shared/kit/constants';
import type { SqliteStore } from '../sqliteStore';
import {
  ComputerUseRuntimePlatform,
  ComputerUseRuntimeTarget,
  getCurrentComputerUseRuntimeDescriptor,
  isComputerUseRuntimeSupportedPlatform,
} from './computerUseRuntime';

const SKILLS_DIR_NAME = 'SKILLs';
const SKILL_FILE_NAME = 'SKILL.md';
const SKILL_STATE_KEY = 'skills_state';
const COMPUTER_USE_RESOURCE_DIR = 'computer-use';
const KIT_ARCHIVE_ENV = 'LOBSTER_COMPUTER_USE_KIT_ARCHIVE';
const COMPUTER_USE_KIT_ICON_URL = 'https://ydhardwarecommon.nosdn.127.net/c0c9390a70b99645de82a673a66d5ae1.png';
const COMPUTER_USE_MCP_REF = {
  id: ComputerUseKitId.BuiltIn,
  name: 'Computer Use',
  description: 'Built-in local desktop control MCP server.',
};

type InstalledKitsMap = Record<string, InstalledKitRecord>;
type SkillStateMap = Record<string, { enabled: boolean }>;

export interface ComputerUseKitBundleDescriptor {
  archiveName?: string;
  bundle: ComputerUseKitBundle;
  sha256: string;
  sizeBytes: number;
}

const MAC_OS_KIT_BUNDLE: ComputerUseKitBundleDescriptor = {
  bundle: ComputerUseKitBundle.MacOS,
  sha256: ComputerUseKitBundleIntegrity.MacOS.Sha256,
  sizeBytes: ComputerUseKitBundleIntegrity.MacOS.SizeBytes,
};

/**
 * Only the targets that fetch a kit appear here. Linux has no download: its helper is compiled from
 * `resources/computer-use-linux` and shipped inside the app, so there is nothing to verify or
 * unpack, and the absence of an entry is what says so.
 */
const ComputerUseKitBundlesByRuntimeTarget: Partial<Record<ComputerUseRuntimeTarget, ComputerUseKitBundleDescriptor>> = {
  [ComputerUseRuntimeTarget.MacArm64]: MAC_OS_KIT_BUNDLE,
  [ComputerUseRuntimeTarget.MacX64]: MAC_OS_KIT_BUNDLE,
  [ComputerUseRuntimeTarget.WindowsX64]: {
    bundle: ComputerUseKitBundle.WindowsX64,
    sha256: ComputerUseKitBundleIntegrity.WindowsX64.Sha256,
    sizeBytes: ComputerUseKitBundleIntegrity.WindowsX64.SizeBytes,
  },
} as const satisfies Partial<Record<ComputerUseRuntimeTarget, ComputerUseKitBundleDescriptor>>;

function isFile(filePath: string): boolean {
  try {
    return fs.statSync(filePath).isFile();
  } catch {
    return false;
  }
}

export function isComputerUseKitSupportedPlatform(): boolean {
  if (!isComputerUseRuntimeSupportedPlatform()) return false;
  const descriptor = getCurrentComputerUseKitBundleDescriptor();
  // Linux ships its helper inside the app and has no downloadable bundle, so the
  // missing descriptor entry must not read as unsupported there.
  if (!descriptor) return true;
  return !descriptor.archiveName || resolveBundledComputerUseKitArchivePath(descriptor) !== null;
}

export function getCurrentComputerUseKitBundleDescriptor(): ComputerUseKitBundleDescriptor | null {
  const runtime = getCurrentComputerUseRuntimeDescriptor();
  if (!runtime) return null;
  return ComputerUseKitBundlesByRuntimeTarget[runtime.target] ?? null;
}

export function resolveBundledComputerUseKitArchivePath(
  descriptor: ComputerUseKitBundleDescriptor,
): string | null {
  const envArchivePath = process.env[KIT_ARCHIVE_ENV]?.trim();
  if (envArchivePath && isFile(envArchivePath)) {
    return envArchivePath;
  }
  if (!descriptor.archiveName) {
    return null;
  }

  const candidates = [
    path.join(process.resourcesPath ?? '', COMPUTER_USE_RESOURCE_DIR, descriptor.archiveName),
    path.join(app.getAppPath(), 'resources', COMPUTER_USE_RESOURCE_DIR, descriptor.archiveName),
    path.join(process.cwd(), 'resources', COMPUTER_USE_RESOURCE_DIR, descriptor.archiveName),
  ];

  return candidates.find(candidate => isFile(candidate)) ?? null;
}

export function buildComputerUseMarketplaceKit(): Record<string, unknown> {
  const runtime = getCurrentComputerUseRuntimeDescriptor();
  const bundleDescriptor = getCurrentComputerUseKitBundleDescriptor();
  return {
    id: ComputerUseKitId.BuiltIn,
    name: ComputerUseKitMetadata.Name,
    description: ComputerUseKitMetadata.Description,
    icon: COMPUTER_USE_KIT_ICON_URL,
    author: 'LobsterAI',
    version: runtime?.version ?? '0.0.0',
    tryAsking: [
      {
        en: 'Open a desktop app and type a short note',
        zh: '打开一个桌面应用并输入一段简短笔记',
      },
      {
        en: 'List the desktop applications I can control',
        zh: '列出可以操作的桌面应用',
      },
    ],
    skills: {
      bundle: bundleDescriptor?.bundle ?? ComputerUseKitBundle.WindowsX64,
      bundleSha256: bundleDescriptor?.sha256 ?? ComputerUseKitBundleIntegrity.WindowsX64.Sha256,
      bundleSizeBytes: bundleDescriptor?.sizeBytes ?? ComputerUseKitBundleIntegrity.WindowsX64.SizeBytes,
      list: [
        {
          id: ComputerUseSkillId.BuiltIn,
          name: ComputerUseKitMetadata.SkillName,
          description: ComputerUseKitMetadata.SkillDescription,
        },
      ],
    },
    mcpServers: [COMPUTER_USE_MCP_REF],
    connectors: [],
  };
}

export function getInstalledKitsMap(store: SqliteStore): InstalledKitsMap {
  return store.get<InstalledKitsMap>(KitStoreKey.Installed) ?? {};
}

export function isComputerUseKitInstalled(store: SqliteStore): boolean {
  return isComputerUseKitSupportedPlatform()
    && Boolean(getInstalledKitsMap(store)[ComputerUseKitId.BuiltIn]);
}

export function buildInstalledComputerUseKitRecord(
  skillIds: string[],
  metadata: Record<string, KitSkillMetadata>,
): InstalledKitRecord {
  const runtime = getCurrentComputerUseRuntimeDescriptor();
  const skills: InstalledKitSkills = {
    skillIds,
    ...(Object.keys(metadata).length > 0 ? { metadata } : {}),
  };
  return {
    id: ComputerUseKitId.BuiltIn,
    version: runtime?.version ?? '0.0.0',
    installedAt: Date.now(),
    skills,
    mcpServers: [COMPUTER_USE_MCP_REF],
    connectors: [],
  };
}

function getUserComputerUseSkillDir(): string {
  return path.join(app.getPath('userData'), SKILLS_DIR_NAME, ComputerUseSkillId.BuiltIn);
}

/**
 * Refreshes the installed Computer Use skill from the skill copy shipped inside the
 * runtime, and moves the kit record to the runtime version, so runtime upgrades do
 * not leave a stale SKILL.md or a "reinstall required" notice behind.
 */
export function syncComputerUseSkillFromRuntime(
  store: SqliteStore,
  runtimeSkillDir: string | undefined,
): boolean {
  const runtime = getCurrentComputerUseRuntimeDescriptor();
  const installedMap = getInstalledKitsMap(store);
  const record = installedMap[ComputerUseKitId.BuiltIn];
  if (!runtime || !record || !runtimeSkillDir) {
    return false;
  }
  const sourceSkill = path.join(runtimeSkillDir, SKILL_FILE_NAME);
  if (!isFile(sourceSkill)) {
    return false;
  }
  const skillId = record.skills?.skillIds?.[0] ?? ComputerUseSkillId.BuiltIn;
  const targetDir = path.join(app.getPath('userData'), SKILLS_DIR_NAME, skillId);
  const targetSkill = path.join(targetDir, SKILL_FILE_NAME);
  const source = fs.readFileSync(sourceSkill, 'utf8');
  const current = isFile(targetSkill) ? fs.readFileSync(targetSkill, 'utf8') : '';
  let changed = false;
  if (current !== source) {
    fs.mkdirSync(targetDir, { recursive: true });
    fs.writeFileSync(targetSkill, source, 'utf8');
    changed = true;
    console.log(`[ComputerUseKit] refreshed skill ${skillId} from runtime ${runtime.version}`);
  }
  if (record.version !== runtime.version) {
    installedMap[ComputerUseKitId.BuiltIn] = { ...record, version: runtime.version };
    store.set(KitStoreKey.Installed, installedMap);
    changed = true;
  }
  return changed;
}

export function removeComputerUseSkillArtifacts(store: SqliteStore): void {
  fs.rmSync(getUserComputerUseSkillDir(), { recursive: true, force: true });
  const stateMap = store.get<SkillStateMap>(SKILL_STATE_KEY) ?? {};
  delete stateMap[ComputerUseSkillId.BuiltIn];
  store.set(SKILL_STATE_KEY, stateMap);
}

// The Linux helper archive carries no skill bundle, so this is the built-in
// skill copy that the kit installer writes into the user SKILLs directory.
const LINUX_COMPUTER_USE_SKILL_MD = `---
name: computer-use
description: Use when a Linux desktop application must be inspected or controlled through LobsterAI Computer Use MCP tools, including screenshots, accessibility text, clicks, typing, key presses, scrolling, or dragging.
official: true
---

# Computer Use (Linux)

Use this skill to automate Linux desktop apps through LobsterAI's built-in Computer Use MCP tools. The helper reads the UI over AT-SPI and injects input over X11/XTest for XWayland windows.

Only use this skill on Linux. If the \`computer-use__\` tools are unavailable, report that Computer Use is unavailable in this session.

Prefer the browser tool for normal web browsing. Use Computer Use for native Linux apps or for browser-adjacent UI that cannot be handled by the browser tool.

The Linux runtime is installed by LobsterAI into its application data runtime directory. It is an internal MCP bridge dependency; do not call the helper executable directly from shell commands.

## Tool Surface

The tools are exposed through MCP with the \`computer-use__\` prefix:

- \`computer-use__list_apps\`
- \`computer-use__list_windows\`
- \`computer-use__launch_app\`
- \`computer-use__get_window\`
- \`computer-use__get_window_state\`
- \`computer-use__activate_window\`
- \`computer-use__click\`
- \`computer-use__press_key\`
- \`computer-use__type_text\`
- \`computer-use__scroll\`
- \`computer-use__drag\`
- \`computer-use__set_value\`
- \`computer-use__perform_secondary_action\`

## Core Workflow

Start with \`computer-use__list_apps\` unless the task is specifically about currently open windows. Use \`computer-use__list_windows\` only when the target app is already running or you need a flat list of open windows.

Choose a target app and window from returned objects. Do not guess or reconstruct a window object.

Before the first snapshot, call \`computer-use__get_window\` on the selected window to refresh the handle. Use \`computer-use__activate_window\` when you need to bring the app foreground before inspection; input tools automatically activate the target.

Use \`computer-use__get_window_state\` to inspect the app. After every state capture, use the returned \`window\` object for later actions because it is the canonical captured window.

Use coordinates from the latest screenshot. Coordinates are window-relative pixels with \`(0, 0)\` at the top-left of the captured window.

Batch related input actions when the UI is stable, then verify once with \`get_window_state\`. Do not take a new screenshot after every click or key press unless focus, layout, modality, or the task state may have changed.

## Input Rules

Use \`computer-use__type_text\` only for literal text. Use \`computer-use__press_key\` for Enter, Tab, Escape, arrows, function keys, and keyboard shortcuts.

Prefer key names such as \`Return\`, \`Tab\`, \`Escape\`, \`Control_L+a\`, and \`Control_L+Shift_L+period\`. Do not use Meta/Super/Window-key shortcuts.

For scrolling, use \`computer-use__scroll\` with a coordinate inside the pane to scroll. If a pane needs focus, click a stable point inside it first, then scroll.

For text entry into editors, documents, slides, sheets, forms, or canvas-like apps, first focus the actual editable surface with a coordinate click or element action, then type. Verify visible output before claiming success.

If a tool says the user denied access, stopped Computer Use, the desktop is locked, or Computer Use is unavailable, stop immediately and report that state.

## Linux Safety

Do not automate terminal applications, command prompts, shells, the LobsterAI desktop app, Cowork/OpenClaw control UIs, password managers, lock screens, authentication dialogs, CAPTCHA, payment flows, or security/privacy settings.

Do not solve CAPTCHAs, bypass web or browser safety interstitials, bypass paywalls, submit password-change final steps, or interact with locked desktop screens.

## Confirmation Policy

Computer Use can cause external side effects. Ask the user for action-time confirmation before:

- deleting cloud or local data through an app UI
- submitting forms, messages, posts, comments, reservations, applications, medical, legal, HR, tax, or financial information
- uploading files or transmitting sensitive data
- changing permissions, sharing settings, account settings, privacy settings, or security settings
- creating accounts, API keys, OAuth keys, persistent credentials, or saved payment/password records
- installing software, running newly downloaded software, or installing browser extensions
- making, scheduling, canceling, or confirming financial transactions

When confirmation is needed, describe the exact action, destination app/site/account, and data involved.
`;

/**
 * Linux: the helper archive carries no skill bundle, so write the skill copy the
 * marketplace kit would otherwise unpack. Returns the installed skill id.
 */
export function writeComputerUseLinuxSkill(): string {
  const targetDir = getUserComputerUseSkillDir();
  fs.mkdirSync(targetDir, { recursive: true });
  fs.writeFileSync(path.join(targetDir, SKILL_FILE_NAME), LINUX_COMPUTER_USE_SKILL_MD, 'utf8');
  return ComputerUseSkillId.BuiltIn;
}

/**
 * macOS: ask for Accessibility right after the kit is installed, so the system prompt
 * appears while the user is looking at Computer Use rather than mid-task. The helper
 * inherits LobsterAI's permission, and requests Screen Recording on first capture.
 */
export function promptComputerUseAccessibilityPermission(): boolean {
  if (process.platform !== ComputerUseRuntimePlatform.MacOS) {
    return true;
  }
  try {
    if (systemPreferences.isTrustedAccessibilityClient(false)) {
      return true;
    }
    systemPreferences.isTrustedAccessibilityClient(true);
  } catch (error) {
    console.warn('[ComputerUseKit] failed to request macOS Accessibility permission:', error);
  }
  return false;
}
