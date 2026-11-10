// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.Context
import android.text.Html
import androidx.preference.PreferenceManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.yuzu.yuzu_emu.R

/**
 * Fritchii's "What's new" window (desktop: fritchii/whats_new.cpp): the release notes of this
 * version, bundled from src/yuzu/fritchii/whats-new.md, shown once after an update and from the
 * app settings.
 */
object WhatsNew {
    private const val ASSET = "whats-new.md"
    private const val PREF_SHOWN = "fritchii_whats_new_shown"

    /** The notes as players read them: the download instructions at the top of a GitHub release
     *  and the "# Packages" tail are dropped. Empty while the file holds only its placeholder. */
    fun notes(context: Context): String {
        val raw = try {
            context.assets.open(ASSET).bufferedReader().use { it.readText() }
        } catch (e: Exception) {
            return ""
        }
        var text = raw.replace(Regex("<!--.*?-->", RegexOption.DOT_MATCHES_ALL), "").trim()
        if (text.startsWith("## ")) {
            val rule = text.indexOf("\n---")
            if (rule >= 0) {
                text = text.substring(rule + 4).trim()
            }
        }
        val packages = text.indexOf("\n# Packages")
        if (packages >= 0) {
            text = text.substring(0, packages).trim()
        }
        return text
    }

    fun showIfNew(context: Context) {
        val text = notes(context)
        if (text.isEmpty()) {
            return
        }
        val preferences = PreferenceManager.getDefaultSharedPreferences(context)
        val stamp = text.hashCode()
        if (preferences.getInt(PREF_SHOWN, 0) == stamp) {
            return
        }
        preferences.edit().putInt(PREF_SHOWN, stamp).apply()
        show(context)
    }

    fun show(context: Context) {
        val text = notes(context)
        MaterialAlertDialogBuilder(context)
            .setTitle(R.string.whats_new)
            .setMessage(
                if (text.isEmpty()) {
                    context.getString(R.string.whats_new_none)
                } else {
                    Html.fromHtml(toHtml(text), Html.FROM_HTML_MODE_COMPACT)
                }
            )
            .setPositiveButton(R.string.close, null)
            .show()
    }

    /** The Markdown the notes use: headings, bold, bullets and inline code. */
    private fun toHtml(markdown: String): String {
        val escape = { line: String ->
            line.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        }
        val inline = { line: String ->
            escape(line)
                .replace(Regex("\\*\\*(.+?)\\*\\*"), "<b>$1</b>")
                .replace(Regex("`([^`]+)`"), "<tt>$1</tt>")
                .replace(Regex("\\[([^\\]]+)\\]\\([^)]+\\)"), "$1")
        }
        return markdown.lines().joinToString("<br>") { line ->
            val trimmed = line.trim()
            when {
                trimmed.startsWith("#") -> "<br><b>" + inline(trimmed.trimStart('#').trim()) + "</b>"
                trimmed.startsWith("- ") -> "&#8226; " + inline(trimmed.substring(2))
                trimmed == "---" -> ""
                else -> inline(line)
            }
        }
    }
}
