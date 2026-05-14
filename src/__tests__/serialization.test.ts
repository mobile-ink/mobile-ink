/**
 * Tests for notebook data serialization/deserialization
 *
 * These tests ensure data integrity during:
 * - Round-trip serialization (serialize -> deserialize)
 * - Multi-page notebooks with varying data sizes
 * - Edge cases (empty data, missing fields)
 *
 * CRITICAL: These tests prevent data loss bugs like the one where
 * page data was being overwritten during file switches.
 */

import {
  serializeNotebookData,
  deserializeNotebookData,
  reconcilePdfNotebookPageCount,
} from '../utils/serialization';
import { NotebookPage } from '../types';

describe('serializeNotebookData', () => {
  it('serializes single page notebook', () => {
    const pages: NotebookPage[] = [
      { id: 'page-1', title: 'Page 1', rotation: 0, data: 'base64data123' }
    ];

    const result = serializeNotebookData(pages);
    const parsed = JSON.parse(result);

    expect(parsed.version).toBe('1.0');
    expect(parsed.pages).toHaveLength(1);
    expect(parsed.pages[0].data).toBe('base64data123');
  });

  it('serializes multi-page notebook preserving all page data', () => {
    const pages: NotebookPage[] = [
      { id: 'page-1', title: 'Page 1', rotation: 0, data: 'page1data' },
      { id: 'page-2', title: 'Page 2', rotation: 0, data: 'page2data' },
      { id: 'page-3', title: 'Page 3', rotation: 0, data: 'page3data' },
      { id: 'page-4', title: 'Page 4', rotation: 0, data: 'page4data' },
      { id: 'page-5', title: 'Page 5', rotation: 0, data: 'page5data' },
    ];

    const result = serializeNotebookData(pages);
    const parsed = JSON.parse(result);

    expect(parsed.pages).toHaveLength(5);
    expect(parsed.pages[0].data).toBe('page1data');
    expect(parsed.pages[1].data).toBe('page2data');
    expect(parsed.pages[2].data).toBe('page3data');
    expect(parsed.pages[3].data).toBe('page4data');
    expect(parsed.pages[4].data).toBe('page5data');
  });

  it('preserves canvas width for cross-device scaling', () => {
    const pages: NotebookPage[] = [
      { id: 'page-1', title: 'Page 1', rotation: 0, data: 'data' }
    ];

    const result = serializeNotebookData(pages, 768);
    const parsed = JSON.parse(result);

    expect(parsed.originalCanvasWidth).toBe(768);
  });

  it('handles pages with varying data sizes', () => {
    // Simulate real-world scenario where pages have different amounts of drawing
    const smallData = 'a'.repeat(100);
    const mediumData = 'b'.repeat(10000);
    const largeData = 'c'.repeat(100000);

    const pages: NotebookPage[] = [
      { id: 'page-1', title: 'Page 1', rotation: 0, data: smallData },
      { id: 'page-2', title: 'Page 2', rotation: 0, data: mediumData },
      { id: 'page-3', title: 'Page 3', rotation: 0, data: largeData },
    ];

    const result = serializeNotebookData(pages);
    const parsed = JSON.parse(result);

    expect(parsed.pages[0].data).toBe(smallData);
    expect(parsed.pages[1].data).toBe(mediumData);
    expect(parsed.pages[2].data).toBe(largeData);
  });

  it('handles pages without data (new blank pages)', () => {
    const pages: NotebookPage[] = [
      { id: 'page-1', title: 'Page 1', rotation: 0, data: 'hasdata' },
      { id: 'page-2', title: 'Page 2', rotation: 0 }, // No data field
      { id: 'page-3', title: 'Page 3', rotation: 0, data: undefined },
    ];

    const result = serializeNotebookData(pages);
    const parsed = JSON.parse(result);

    expect(parsed.pages[0].data).toBe('hasdata');
    expect(parsed.pages[1].data).toBeUndefined();
    expect(parsed.pages[2].data).toBeUndefined();
  });
});

describe('deserializeNotebookData', () => {
  it('deserializes single page notebook', () => {
    const json = JSON.stringify({
      version: '1.0',
      pages: [{ id: 'page-1', title: 'Page 1', rotation: 0, data: 'testdata' }]
    });

    const result = deserializeNotebookData(json);

    expect(result.pages).toHaveLength(1);
    expect(result.pages[0].data).toBe('testdata');
  });

  it('deserializes multi-page notebook preserving all page data', () => {
    const json = JSON.stringify({
      version: '1.0',
      pages: [
        { id: 'page-1', title: 'Page 1', rotation: 0, data: 'data1' },
        { id: 'page-2', title: 'Page 2', rotation: 0, data: 'data2' },
        { id: 'page-3', title: 'Page 3', rotation: 0, data: 'data3' },
      ]
    });

    const result = deserializeNotebookData(json);

    expect(result.pages).toHaveLength(3);
    expect(result.pages[0].data).toBe('data1');
    expect(result.pages[1].data).toBe('data2');
    expect(result.pages[2].data).toBe('data3');
  });

  it('extracts original canvas width', () => {
    const json = JSON.stringify({
      version: '1.0',
      pages: [{ id: 'page-1', title: 'Page 1', rotation: 0 }],
      originalCanvasWidth: 1024
    });

    const result = deserializeNotebookData(json);

    expect(result.originalCanvasWidth).toBe(1024);
  });

  it('handles invalid JSON gracefully', () => {
    const result = deserializeNotebookData('not valid json');

    expect(result.pages).toEqual([]);
  });

  it('handles missing version gracefully', () => {
    const json = JSON.stringify({
      pages: [{ id: 'page-1', title: 'Page 1', rotation: 0 }]
    });

    const result = deserializeNotebookData(json);

    expect(result.pages).toEqual([]);
  });

  it('handles empty pages array gracefully', () => {
    const json = JSON.stringify({
      version: '1.0',
      pages: []
    });

    const result = deserializeNotebookData(json);

    expect(result.pages).toEqual([]);
  });
});

describe('serialization round-trip', () => {
  it('preserves all page data through serialize/deserialize cycle', () => {
    const originalPages: NotebookPage[] = [
      { id: 'page-1', title: 'Page 1', rotation: 0, data: 'drawing1' },
      { id: 'page-2', title: 'Page 2', rotation: 90, data: 'drawing2' },
      { id: 'page-3', title: 'Page 3', rotation: 180, data: 'drawing3' },
      { id: 'page-4', title: 'Page 4', rotation: 270, data: 'drawing4' },
    ];
    const canvasWidth = 834;

    const serialized = serializeNotebookData(originalPages, canvasWidth);
    const { pages: deserializedPages, originalCanvasWidth } = deserializeNotebookData(serialized);

    expect(deserializedPages).toHaveLength(4);
    expect(originalCanvasWidth).toBe(canvasWidth);

    // CRITICAL: Verify each page's data is preserved
    for (let i = 0; i < originalPages.length; i++) {
      expect(deserializedPages[i].id).toBe(originalPages[i].id);
      expect(deserializedPages[i].data).toBe(originalPages[i].data);
      expect(deserializedPages[i].rotation).toBe(originalPages[i].rotation);
    }
  });

  it('preserves large data through round-trip (simulating real drawings)', () => {
    // Simulate base64 encoded drawing data of ~100KB per page
    const generateLargeData = (size: number) => {
      const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=';
      let result = '';
      for (let i = 0; i < size; i++) {
        result += chars.charAt(Math.floor(Math.random() * chars.length));
      }
      return result;
    };

    const originalPages: NotebookPage[] = [
      { id: 'page-1', title: 'Page 1', rotation: 0, data: generateLargeData(50000) },
      { id: 'page-2', title: 'Page 2', rotation: 0, data: generateLargeData(100000) },
      { id: 'page-3', title: 'Page 3', rotation: 0, data: generateLargeData(75000) },
    ];

    const serialized = serializeNotebookData(originalPages);
    const { pages: deserializedPages } = deserializeNotebookData(serialized);

    // Verify data integrity
    expect(deserializedPages[0].data).toBe(originalPages[0].data);
    expect(deserializedPages[1].data).toBe(originalPages[1].data);
    expect(deserializedPages[2].data).toBe(originalPages[2].data);

    // Verify lengths match exactly
    expect(deserializedPages[0].data?.length).toBe(50000);
    expect(deserializedPages[1].data?.length).toBe(100000);
    expect(deserializedPages[2].data?.length).toBe(75000);
  });

  it('preserves text boxes through round-trip', () => {
    const originalPages: NotebookPage[] = [
      {
        id: 'page-1',
        title: 'Page 1',
        rotation: 0,
        data: 'drawing',
        textBoxes: [
          {
            id: 'tb1',
            content: 'Hello',
            x: 100,
            y: 200,
            width: 150,
            height: 50,
            fontSize: 16,
            color: '#000',
            isEditing: false,
          }
        ]
      }
    ];

    const serialized = serializeNotebookData(originalPages);
    const { pages: deserializedPages } = deserializeNotebookData(serialized);

    expect(deserializedPages[0].textBoxes).toBeDefined();
    expect(deserializedPages[0].textBoxes?.[0].content).toBe('Hello');
    expect(deserializedPages[0].textBoxes?.[0].x).toBe(100);
  });

  it('preserves inserted element geometry, crop, and rendered figure data through round-trip', () => {
    const originalPages: NotebookPage[] = [
      {
        id: 'page-1',
        title: 'Page 1',
        rotation: 0,
        data: 'drawing',
        insertedElements: [
          {
            id: 'image-1',
            type: 'image',
            sourceUri: 'file:///persisted/image.png',
            aspectRatio: 1.5,
            cropRect: { x: 0.1, y: 0.2, width: 0.7, height: 0.6 },
            x: 40,
            y: 50,
            width: 210,
            height: 180,
            rotation: 12,
            locked: false,
            zIndex: 10,
            createdAt: '2026-05-06T00:00:00.000Z',
            updatedAt: '2026-05-06T00:01:00.000Z',
          },
          {
            id: 'figure-1',
            type: 'latex-figure',
            tikzCode: '\\begin{tikzpicture}\\draw (0,0)--(1,1);\\end{tikzpicture}',
            renderedImageUri: 'data:application/pdf;base64,JVBERi0=',
            renderStatus: 'ready',
            x: 80,
            y: 90,
            width: 160,
            height: 120,
            rotation: -8,
            locked: false,
            zIndex: 11,
            createdAt: '2026-05-06T00:00:00.000Z',
            updatedAt: '2026-05-06T00:01:00.000Z',
          },
        ],
      },
    ];

    const serialized = serializeNotebookData(originalPages);
    const { pages: deserializedPages } = deserializeNotebookData(serialized);

    expect(deserializedPages[0].insertedElements).toEqual(originalPages[0].insertedElements);
  });

  it('preserves PDF page numbers through round-trip', () => {
    const originalPages: NotebookPage[] = [
      { id: 'page-1', title: 'Page 1', rotation: 0, data: 'd1', pdfPageNumber: 1 },
      { id: 'page-2', title: 'Page 2', rotation: 0, data: 'd2', pdfPageNumber: 2 },
      { id: 'page-3', title: 'Page 3', rotation: 0, data: 'd3', pdfPageNumber: 3 },
    ];

    const serialized = serializeNotebookData(originalPages);
    const { pages: deserializedPages } = deserializeNotebookData(serialized);

    expect(deserializedPages[0].pdfPageNumber).toBe(1);
    expect(deserializedPages[1].pdfPageNumber).toBe(2);
    expect(deserializedPages[2].pdfPageNumber).toBe(3);
  });

  it('preserves page preview metadata through round-trip', () => {
    const originalPages: NotebookPage[] = [
      {
        id: 'page-1',
        title: 'Page 1',
        rotation: 0,
        data: 'drawing',
        dataSignature: '7:drawing',
        previewUri: 'data:image/png;base64,preview',
        previewDataSignature: '7:drawing',
      },
    ];

    const serialized = serializeNotebookData(originalPages);
    const { pages: deserializedPages } = deserializeNotebookData(serialized);

    expect(deserializedPages[0].previewUri).toBe('data:image/png;base64,preview');
    expect(deserializedPages[0].previewDataSignature).toBe('7:drawing');
  });
});

describe('data integrity edge cases', () => {
  it('handles page with only some fields populated', () => {
    const pages: NotebookPage[] = [
      { id: 'page-1', title: 'Page 1', rotation: 0 }, // Minimal page
    ];

    const serialized = serializeNotebookData(pages);
    const { pages: result } = deserializeNotebookData(serialized);

    expect(result[0].id).toBe('page-1');
    expect(result[0].data).toBeUndefined();
  });

  it('preserves empty string data (different from undefined)', () => {
    const pages: NotebookPage[] = [
      { id: 'page-1', title: 'Page 1', rotation: 0, data: '' },
    ];

    const serialized = serializeNotebookData(pages);
    const { pages: result } = deserializeNotebookData(serialized);

    // Empty string should be preserved, not converted to undefined
    expect(result[0].data).toBe('');
  });

  it('handles special characters in data', () => {
    const specialData = 'data with "quotes" and \\backslashes\\ and\nnewlines';
    const pages: NotebookPage[] = [
      { id: 'page-1', title: 'Page 1', rotation: 0, data: specialData },
    ];

    const serialized = serializeNotebookData(pages);
    const { pages: result } = deserializeNotebookData(serialized);

    expect(result[0].data).toBe(specialData);
  });
});

describe('reconcilePdfNotebookPageCount', () => {
  it('appends only missing PDF pages and preserves existing page content', () => {
    const existingPages: NotebookPage[] = [
      {
        id: 'page-1',
        title: 'Homework 1',
        rotation: 90,
        data: 'drawing-a',
        pdfPageNumber: 1,
        textBoxes: [
          {
            id: 'tb-1',
            x: 12,
            y: 24,
            width: 120,
            content: 'annotated',
            color: '#111111',
            isEditing: false,
          },
        ],
      },
      {
        id: 'page-2',
        title: 'Homework 2',
        rotation: 0,
        data: 'drawing-b',
        pdfPageNumber: 2,
      },
    ];

    const result = reconcilePdfNotebookPageCount(
      serializeNotebookData(existingPages, 834),
      4
    );

    expect(result.changed).toBe(true);
    expect(result.reasonCode).toBe('appended_missing_pdf_pages');
    expect(result.pages).toHaveLength(4);
    expect(result.pages[0]).toMatchObject(existingPages[0]);
    expect(result.pages[1]).toMatchObject(existingPages[1]);
    expect(result.pages[2]).toMatchObject({
      id: 'page-3',
      title: 'Page 3',
      data: '',
      rotation: 0,
      pdfPageNumber: 3,
    });
    expect(result.pages[3]).toMatchObject({
      id: 'page-4',
      title: 'Page 4',
      data: '',
      rotation: 0,
      pdfPageNumber: 4,
    });
  });

  it('leaves richer notebooks unchanged when page count is already sufficient', () => {
    const notebookData = serializeNotebookData([
      { id: 'page-1', title: 'Page 1', rotation: 0, data: 'a', pdfPageNumber: 1 },
      { id: 'page-2', title: 'Page 2', rotation: 0, data: 'b', pdfPageNumber: 2 },
      { id: 'page-3', title: 'Page 3', rotation: 0, data: 'c', pdfPageNumber: 3 },
    ], 768);

    const result = reconcilePdfNotebookPageCount(notebookData, 2);

    expect(result.changed).toBe(false);
    expect(result.reasonCode).toBe('existing_pages_sufficient');
    expect(result.notebookData).toBe(notebookData);
    expect(result.pages).toHaveLength(3);
    expect(result.pages[2].data).toBe('c');
  });

  it('creates blank PDF placeholders when no notebook exists yet', () => {
    const result = reconcilePdfNotebookPageCount('', 3);

    expect(result.changed).toBe(true);
    expect(result.reasonCode).toBe('created_pdf_placeholders');
    expect(result.pages).toHaveLength(3);
    expect(result.pages[0].pdfPageNumber).toBe(1);
    expect(result.pages[2].pdfPageNumber).toBe(3);
    expect(result.pages.every((page) => page.data === '')).toBe(true);
  });
});
