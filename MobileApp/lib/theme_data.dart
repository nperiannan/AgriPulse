import 'package:flutter/material.dart';

// ─── Dark Theme Colours ──────────────────────────────────────────────────────
const kDarkBg     = Color(0xFF0f1117);
const kDarkCard   = Color(0xFF1a1d27);
const kDarkCardBd = Color(0xFF262a36);
const kDarkLabel  = Color(0xFF8b8fa3);

// ─── Light Theme Colours ─────────────────────────────────────────────────────
const kLightBg     = Color(0xFFF0F4F8);
const kLightCard   = Color(0xFFE8EEF4);
const kLightCardBd = Color(0xFFB8C9DB);
const kLightLabel  = Color(0xFF6B7B8D);

// ─── Shared accent colours (from concept mockup) ─────────────────────────────
const kBlue   = Color(0xFF448aff);
const kGreen  = Color(0xFF00e676);
const kOrange = Color(0xFFffab40);
const kRed    = Color(0xFFff5252);

// Light-mode accent variants
const kLightGreen  = Color(0xFF1976D2);   // blue replaces green in light theme
const kLightRed    = Color(0xFFd32f2f);
const kLightBlue   = Color(0xFF1565c0);
const kLightOrange = Color(0xFFe65100);

/// Dark theme data.
final ThemeData darkTheme = ThemeData.dark().copyWith(
  scaffoldBackgroundColor: kDarkBg,
  colorScheme: const ColorScheme.dark(primary: kBlue),
  cardColor: kDarkCard,
  dividerColor: kDarkCardBd,
  switchTheme: SwitchThemeData(
    thumbColor: WidgetStateProperty.resolveWith(
        (s) => s.contains(WidgetState.selected) ? kBlue : null),
    trackColor: WidgetStateProperty.resolveWith(
        (s) => s.contains(WidgetState.selected) ? kBlue.withOpacity(0.4) : null),
  ),
  bottomNavigationBarTheme: const BottomNavigationBarThemeData(
    backgroundColor: kDarkCard,
    selectedItemColor: kBlue,
    unselectedItemColor: kDarkLabel,
  ),
  appBarTheme: const AppBarTheme(
    backgroundColor: kDarkCard,
    elevation: 0,
  ),
);

/// Light theme data.
final ThemeData lightTheme = ThemeData.light().copyWith(
  scaffoldBackgroundColor: kLightBg,
  colorScheme: const ColorScheme.light(primary: kLightBlue),
  cardColor: kLightCard,
  dividerColor: kLightCardBd,
  switchTheme: SwitchThemeData(
    thumbColor: WidgetStateProperty.resolveWith(
        (s) => s.contains(WidgetState.selected) ? kLightBlue : null),
    trackColor: WidgetStateProperty.resolveWith(
        (s) => s.contains(WidgetState.selected) ? kLightBlue.withOpacity(0.3) : null),
  ),
  bottomNavigationBarTheme: const BottomNavigationBarThemeData(
    backgroundColor: kLightCard,
    selectedItemColor: kLightBlue,
    unselectedItemColor: kLightLabel,
  ),
  appBarTheme: const AppBarTheme(
    backgroundColor: kLightCard,
    elevation: 0,
    iconTheme: IconThemeData(color: Color(0xFF333333)),
    titleTextStyle: TextStyle(color: Color(0xFF1a1a2e), fontSize: 17, fontWeight: FontWeight.w700),
  ),
);

// ─── Helpers to resolve colours from the current theme ───────────────────────
/// Card background that follows light/dark.
Color cardBg(BuildContext context) => Theme.of(context).cardColor;

/// Card border that follows light/dark.
Color cardBd(BuildContext context) => Theme.of(context).dividerColor;

/// Secondary label colour.
Color labelColor(BuildContext context) =>
    Theme.of(context).brightness == Brightness.dark ? kDarkLabel : kLightLabel;

/// Scaffold / page background.
Color scaffoldBg(BuildContext context) =>
    Theme.of(context).scaffoldBackgroundColor;

/// Primary text colour.
Color textColor(BuildContext context) =>
    Theme.of(context).brightness == Brightness.dark
        ? const Color(0xFFE4E6EB)
        : const Color(0xFF1a1a2e);

/// Subtle row background.
Color subtleBg(BuildContext context) =>
    Theme.of(context).brightness == Brightness.dark
        ? const Color(0xFF262a36)
        : const Color(0xFFEAEDF2);

/// Theme-aware accent green.
Color accentGreen(BuildContext context) =>
    Theme.of(context).brightness == Brightness.dark ? kGreen : kLightGreen;

/// Theme-aware accent red.
Color accentRed(BuildContext context) =>
    Theme.of(context).brightness == Brightness.dark ? kRed : kLightRed;

/// Theme-aware accent blue.
Color accentBlue(BuildContext context) =>
    Theme.of(context).brightness == Brightness.dark ? kBlue : kLightBlue;

/// Theme-aware accent orange.
Color accentOrange(BuildContext context) =>
    Theme.of(context).brightness == Brightness.dark ? kOrange : kLightOrange;
