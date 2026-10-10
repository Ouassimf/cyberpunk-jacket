package com.ouassimf.jacket.protocol

import kotlinx.serialization.json.JsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.junit.runners.Parameterized

/** Every golden vector, both ways: `encode(fields) == hex` and `decode(hex) == fields`. */
@RunWith(Parameterized::class)
class GoldenVectorsTest(private val vector: GoldenVector) {

    @Test
    fun `fields encode to exactly the hex`() {
        assertEquals(vector.hex, vector.encode().toHex())
    }

    @Test
    fun `hex decodes to exactly the fields`() {
        assertEquals(vector.fields, GoldenVector.fieldsOf(vector.decode()))
    }

    companion object {
        @JvmStatic
        @Parameterized.Parameters(name = "{0}")
        fun vectors(): List<GoldenVector> = GoldenVector.load()
    }
}

class GoldenVectorCoverageTest {

    /** Guards against the file quietly losing vectors: each frame type of the document has at least one. */
    @Test
    fun `every frame type of the protocol has a vector`() {
        val expected = setOf(
            "SET_POWER", "SET_BRIGHTNESS", "SET_EFFECT", "SET_PARAM", "SET_SEGMENTS", "IDENTIFY",
            "PRESET_SAVE", "PRESET_LOAD", "PRESET_DELETE", "GET_PRESETS", "GET_INFO", "GET_CATALOG",
            "SET_BUDGET", "SET_DEV_MODE", "BOOT_OTHER_SLOT", "STATE", "CATALOG", "PRESETS", "INFO", "ERROR", "LIVE",
        )
        val present = GoldenVector.load().map { it.type }.toSet()
        assertEquals(expected, present)
    }

    @Test
    fun `no vector is longer than the largest frame`() {
        assertTrue(GoldenVector.load().all { it.bytes.size <= Protocol.MAX_FRAME })
    }

    @Test(expected = IllegalStateException::class)
    fun `a vector of an unknown type fails instead of being skipped`() {
        GoldenVector("future", "command", "SET_SOMETHING_NEW", "ff", JsonObject(emptyMap())).frame()
    }
}
