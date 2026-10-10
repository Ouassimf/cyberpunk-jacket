package com.ouassimf.jacket.protocol

/** Size of this string as a protocol `str` payload: its UTF-8 bytes. */
val String.utf8Size: Int get() = encodeToByteArray().size

/**
 * The longest prefix of this string that fits [maxBytes] of UTF-8, never cutting a character in half (a
 * surrogate pair counts as one character). The protocol limits names by bytes, so "16 characters" is not it.
 */
fun String.truncateUtf8(maxBytes: Int): String {
    var bytes = 0
    var index = 0
    while (index < length) {
        val codePoint = codePointAt(index)
        val size = when {
            codePoint < 0x80 -> 1
            codePoint < 0x800 -> 2
            codePoint < 0x10000 -> 3
            else -> 4
        }
        if (bytes + size > maxBytes) break
        bytes += size
        index += Character.charCount(codePoint)
    }
    return substring(0, index)
}
