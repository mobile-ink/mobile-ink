const BLANK_PAGE_PAYLOAD = '{"pages":{}}';

export type NormalizedPagePayload = {
  isValid: boolean;
  normalizedPayload: string;
  reasonCode?: string;
};

export function normalizePagePayloadForNativeLoad(
  pagePayload: string | null | undefined,
): NormalizedPagePayload {
  const trimmedPayload = pagePayload?.trim() ?? "";
  if (!trimmedPayload) {
    return {
      isValid: true,
      normalizedPayload: BLANK_PAGE_PAYLOAD,
      reasonCode: "blank_payload",
    };
  }

  try {
    const parsed = JSON.parse(trimmedPayload);
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
      return {
        isValid: false,
        normalizedPayload: BLANK_PAGE_PAYLOAD,
        reasonCode: "payload_not_object",
      };
    }

    const pages = (parsed as { pages?: unknown }).pages;
    if (!pages || typeof pages !== "object" || Array.isArray(pages)) {
      return {
        isValid: true,
        normalizedPayload: BLANK_PAGE_PAYLOAD,
        reasonCode: "missing_pages",
      };
    }

    return {
      isValid: true,
      normalizedPayload: trimmedPayload,
    };
  } catch {
    return {
      isValid: false,
      normalizedPayload: BLANK_PAGE_PAYLOAD,
      reasonCode: "json_parse_failed",
    };
  }
}
