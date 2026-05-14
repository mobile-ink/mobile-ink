package com.mathnotes.mobileink

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Matrix
import android.graphics.pdf.PdfRenderer
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.util.Base64
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * Utility class for loading PDF files from various sources and rendering them to bitmaps.
 * Supports: content:// (document picker), file://, https://, http://,
 * absolute paths, and data:application/pdf;base64 URIs.
 */
object PdfLoader {

    /**
     * Load a PDF and render it to a bitmap.
     * @param context Android context for content resolver access
     * @param uri PDF URI (supports multiple formats)
     * @param viewWidth Target width for rendering (uses PDF native width if 0)
     * @param viewHeight Target height for rendering (uses PDF native height if 0)
     * @return Rendered bitmap or null on failure
     */
    fun loadAndRenderPdf(context: Context, uri: String, viewWidth: Int, viewHeight: Int): Bitmap? {
        // Parse page number from URI fragment (#page=N, 1-indexed)
        var pageNumber = 0
        var cleanUri = uri

        val hashIndex = uri.indexOf('#')
        if (hashIndex != -1) {
            val fragment = uri.substring(hashIndex + 1)
            if (fragment.startsWith("page=")) {
                pageNumber = (fragment.substringAfter("page=").toIntOrNull() ?: 1) - 1
            }
            cleanUri = uri.substring(0, hashIndex)
        }

        return try {
            when {
                // Handle base64 data URI
                cleanUri.startsWith("data:application/pdf;base64,") -> {
                    loadPdfFromBase64(context, cleanUri, pageNumber, viewWidth, viewHeight)
                }
                // Handle content:// URI (from document picker - Google Drive, Downloads, etc.)
                cleanUri.startsWith("content://") -> {
                    loadPdfFromContentUri(context, cleanUri, pageNumber, viewWidth, viewHeight)
                }
                // Handle https:// and http:// URLs
                cleanUri.startsWith("https://") || cleanUri.startsWith("http://") -> {
                    loadPdfFromUrl(context, cleanUri, pageNumber, viewWidth, viewHeight)
                }
                // Handle file:// URI
                cleanUri.startsWith("file://") -> {
                    loadPdfFromFile(File(cleanUri.removePrefix("file://")), pageNumber, viewWidth, viewHeight)
                }
                // Handle absolute path
                cleanUri.startsWith("/") -> {
                    loadPdfFromFile(File(cleanUri), pageNumber, viewWidth, viewHeight)
                }
                else -> {
                    android.util.Log.e("PdfLoader", "Unsupported PDF URI format")
                    null
                }
            }
        } catch (e: Exception) {
            android.util.Log.e("PdfLoader", "Error loading PDF", e)
            null
        }
    }

    /**
     * Load PDF from a base64 data URI by writing to temp file.
     */
    private fun loadPdfFromBase64(context: Context, dataUri: String, pageNumber: Int, viewWidth: Int, viewHeight: Int): Bitmap? {
        val base64Data = dataUri.removePrefix("data:application/pdf;base64,")
        val pdfBytes = Base64.decode(base64Data, Base64.DEFAULT)

        // PdfRenderer requires a file, so write to temp file
        val tempFile = File.createTempFile("pdf_bg_", ".pdf", context.cacheDir)
        try {
            tempFile.writeBytes(pdfBytes)
            return loadPdfFromFile(tempFile, pageNumber, viewWidth, viewHeight)
        } finally {
            tempFile.delete()
        }
    }

    /**
     * Load PDF from a file and render the specified page to a bitmap.
     */
    private fun loadPdfFromFile(file: File, pageNumber: Int, viewWidth: Int, viewHeight: Int): Bitmap? {
        if (!file.exists()) {
            android.util.Log.e("PdfLoader", "PDF file not found")
            return null
        }

        val pfd = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
        val renderer = PdfRenderer(pfd)

        try {
            // Clamp page number to valid range
            val validPageNumber = pageNumber.coerceIn(0, renderer.pageCount - 1)
            val page = renderer.openPage(validPageNumber)

            try {
                return renderPdfPage(page, viewWidth, viewHeight)
            } finally {
                page.close()
            }
        } finally {
            renderer.close()
            pfd.close()
        }
    }

    /**
     * Load PDF from a content:// URI (from Android document picker).
     */
    private fun loadPdfFromContentUri(context: Context, uriString: String, pageNumber: Int, viewWidth: Int, viewHeight: Int): Bitmap? {
        val uri = Uri.parse(uriString)

        val pfd = context.contentResolver.openFileDescriptor(uri, "r")
            ?: run {
                android.util.Log.e("PdfLoader", "Failed to open content URI")
                return null
            }

        val renderer = PdfRenderer(pfd)

        try {
            val validPageNumber = pageNumber.coerceIn(0, renderer.pageCount - 1)
            val page = renderer.openPage(validPageNumber)

            try {
                return renderPdfPage(page, viewWidth, viewHeight)
            } finally {
                page.close()
            }
        } finally {
            renderer.close()
            pfd.close()
        }
    }

    /**
     * Load PDF from an https:// or http:// URL by downloading to temp file.
     */
    private fun loadPdfFromUrl(context: Context, urlString: String, pageNumber: Int, viewWidth: Int, viewHeight: Int): Bitmap? {
        val tempFile = File.createTempFile("pdf_download_", ".pdf", context.cacheDir)
        try {
            val connection = URL(urlString).openConnection() as HttpURLConnection
            connection.connectTimeout = 30000
            connection.readTimeout = 30000
            connection.instanceFollowRedirects = true

            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                android.util.Log.e("PdfLoader",
                    "Failed to download PDF: HTTP ${connection.responseCode}")
                return null
            }

            connection.inputStream.use { input ->
                tempFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }

            return loadPdfFromFile(tempFile, pageNumber, viewWidth, viewHeight)
        } catch (e: Exception) {
            android.util.Log.e("PdfLoader", "Error downloading PDF", e)
            return null
        } finally {
            tempFile.delete()
        }
    }

    private fun renderPdfPage(page: PdfRenderer.Page, viewWidth: Int, viewHeight: Int): Bitmap {
        val targetWidth = if (viewWidth > 0) viewWidth else page.width
        val targetHeight = if (viewHeight > 0) viewHeight else page.height
        val bitmap = Bitmap.createBitmap(targetWidth, targetHeight, Bitmap.Config.ARGB_8888)
        bitmap.eraseColor(android.graphics.Color.WHITE)

        val fitScale = minOf(
            targetWidth.toFloat() / page.width.toFloat(),
            targetHeight.toFloat() / page.height.toFloat()
        )
        val scaledWidth = page.width * fitScale
        val scaledHeight = page.height * fitScale
        val offsetX = (targetWidth - scaledWidth) / 2f
        val offsetY = (targetHeight - scaledHeight) / 2f
        val matrix = Matrix().apply {
            postScale(fitScale, fitScale)
            postTranslate(offsetX, offsetY)
        }

        page.render(bitmap, null, matrix, PdfRenderer.Page.RENDER_MODE_FOR_DISPLAY)
        return bitmap
    }
}
