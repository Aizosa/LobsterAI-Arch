import { describe, expect, test, vi } from 'vitest';

/**
 * Linux reports a selection without any text geometry: every corner is INVALID_COORDINATE and
 * posLevel never passes MOUSE_DUAL, so the anchor has to come from the pointer. These tests pin the
 * two things that decide where the toolbar lands, using the shape of data measured on KWin 6.8.
 */
const dip = vi.hoisted(() => ({ scale: 1 }));

vi.mock('electron', () => ({
  BrowserWindow: { getFocusedWindow: () => null },
  screen: {
    getCursorScreenPoint: () => ({ x: 400, y: 300 }),
    getDisplayNearestPoint: () => ({ workArea: { x: 0, y: 0, width: 3600, height: 1275 } }),
    // Stands in for X11 fractional scaling: screen coordinates are larger than the DIP layout.
    screenToDipPoint: (point: { x: number, y: number }) => ({ x: point.x / dip.scale, y: point.y / dip.scale }),
  },
  systemPreferences: { isTrustedAccessibilityClient: () => true },
  shell: { openExternal: vi.fn(() => Promise.resolve()) },
  clipboard: { writeText: vi.fn() },
}));
vi.mock('../i18n', () => ({ t: (key: string) => key }));

const { selectionAnchor } = await import('./companionSelection');

const INVALID = -99999;
/** What selection-hook hands over on Linux: text and a pointer, no rectangle. */
function linuxSelection(overrides: Record<string, unknown> = {}) {
  return {
    text: 'selected words',
    programName: '',
    startTop: { x: INVALID, y: INVALID },
    startBottom: { x: INVALID, y: INVALID },
    endTop: { x: INVALID, y: INVALID },
    endBottom: { x: INVALID, y: INVALID },
    mousePosStart: { x: 200, y: 100 },
    mousePosEnd: { x: 200, y: 100 },
    posLevel: 1,
    method: 22,
    ...overrides,
  } as Parameters<typeof selectionAnchor>[0];
}

describe('selectionAnchor on Linux', () => {
  test('anchors on the pointer when the hook reports no rectangle', () => {
    const anchor = selectionAnchor(linuxSelection(), 3, { x: 0, y: 0 });
    expect(anchor.multiline).toBe(false);
    expect(anchor.top).toBeLessThan(100);
    expect(anchor.bottom).toBeGreaterThan(100);
    expect(anchor.left).toBe(200);
    expect(anchor.right).toBe(200);
  });

  test('spans both ends of a drag when the compositor reported them', () => {
    const anchor = selectionAnchor(linuxSelection({ mousePosEnd: { x: 520, y: 240 } }), 3, { x: 0, y: 0 });
    expect(anchor.multiline).toBe(true);
    expect(anchor.top).toBeLessThan(100);
    expect(anchor.bottom).toBeGreaterThan(240);
  });

  test('falls back to the cursor when the compositor gave no pointer either', () => {
    const anchor = selectionAnchor(linuxSelection({ mousePosStart: undefined, mousePosEnd: { x: INVALID, y: INVALID } }), 3, { x: 700, y: 500 });
    expect(anchor.left).toBe(700);
    expect(anchor.right).toBe(700);
  });

  // Linux screen coordinates are not DIP when X11 fractional scaling is on, unlike macOS.
  test('converts screen coordinates to DIP on Linux', () => {
    dip.scale = 2;
    const anchor = selectionAnchor(linuxSelection({ mousePosStart: { x: 400, y: 200 }, mousePosEnd: { x: 400, y: 200 } }), 3, { x: 0, y: 0 });
    // 400/2 and 200/2, plus the line padding the pointer branch adds.
    expect(anchor.left).toBe(200);
    expect(anchor.top).toBe(82);
    expect(anchor.bottom).toBe(114);
    dip.scale = 1;
  });
});
