--[[ User settings for Disciples 2 Rise of the Elves v3.01 mss32 proxy dll

This file contains client-side settings that do not affect gameplay balance.
It is intended for visual, interface and convenience options.

This file is excluded from multiplayer file verification, so players can use
different user settings while remaining network compatible.

Most settings correspond to features. Refer to the documentation for more information:
https://github.com/bartonsun/D2ModdingToolset

If you got this file from the GitHub repository, settings have their default values specified.
If you omit any setting it will have its default value.


]]--

settings = {

	-- Show troops banners
	showBanners = true,

	-- Show resources panel
	showResources = true,

	-- Show percentage of land coverted
	showLandConverted = false,

	-- Maximum number of items the player is allowed to transfer
	-- between campaign scenarios [0 : INT_MAX]
	carryOverItemsMax = 5,

	-- Custom item category order used by:
	-- BTN_SORT_L_CUSTOM
	-- BTN_SORT_R_CUSTOM
	--
	-- Categories are sorted from top to bottom in the exact order listed here.
	--
	-- You may specify only the categories you want to prioritize.
	-- Any categories omitted from this list will use the default internal order
	-- and will be placed after the custom categories.
	--
	-- This section may also be omitted completely.
	-- In that case, the built-in default sort order will be used.
	customSortOrder = {
		"PotionRevive",    -- Resurrection potions
		"PotionHeal",      -- Healing potions

		"Weapon",          -- Weapons and offensive artifacts
		"Armor",           -- Armor and defensive artifacts

		"Jewel",           -- Jewels and rings
		"Banner",          -- Banners and standards

		"PotionBoost",     -- Temporary stat boost potions
		"PotionPermanent", -- Permanent stat increase potions

		"Scroll",          -- Spell scrolls
		"Wand",            -- Magic wands

		"Talisman",        -- Talismans
		"Orb",             -- Orbs

		"Valuable",        -- Valuable trade items
		"Travelitem",      -- Travel and utility items
		"Special"          -- Special quest or unique items
	},

	-- Configurable hotkeys for the strategic map interface.
	-- Defaults:
	-- openSelectedObject = "I"  inventory
	-- quickSave = ctrl + "Q"
	hotkeys = {

		openSelectedObject = { -- inventory from selected stack or city
			key = "I",
			ctrl = false,
			shift = false,
			alt = false,
		},

		quickSave = { -- quick save, works in multiplayer game for host
			key = "Q",
			ctrl = true,
			shift = false,
			alt = false,
		},
		-- Special hotkey used with a strategic map click to build a movement path.
		-- Unlike other hotkeys, it uses only the specified key and does not support
		-- the ctrl, shift, or alt modifier fields.
		-- The generated movement path is drawn above all game objects.
		customPathLayer = {
			key = "SHIFT"
		},
	},

	-- Strategic map movement cost display settings.
	movementCost = {

		-- Show stacks movement cost
		show = true,

		-- Show remaining movement points after an action on the first red flag
		showMovementAfterAction = true,

		-- Color components are all in range [0 : 255]
		textColor = {
			red = 200,
			green = 200,
			blue = 200,
		},

		outlineColor = {
			red = 0,
			green = 0,
			blue = 0,
		},
	},

	lobby = {

		-- Lobby server public IP and port
		server = {
			ip = "104.248.139.25",
			port = 61111,
		},

		client = {
			-- Lobby client port (0 means auto-assign by OS)
			port = 0,
		},

		-- Локальная видимость кнопок; не участвует в сетевой сверке.
		-- Скрытые режимы выключены при создании своей комнаты; вход в чужие не ограничен.
		-- Изменения применяются после перезапуска игры.
		controls = {
			ranked = true, -- Показывать кнопку, а не включать рейтинг автоматически.
			simultaneousTurns = false,
			unlockGui = false,
		},

		-- Начальный выбор при входе в лобби. Повторное открытие окна сохраняет ваш выбор.
		-- Скрытый или отсутствующий в .dlg режим всегда выключен, независимо от defaults.
		defaults = {
			ranked = false,
			simultaneousTurns = false,
			unlockGui = false,
			simultaneousTurnsDays = 7, -- 0..30; число дней само не включает ОХ при наличии кнопки.
		},
	},

	unitEncyclopedia = {

		-- Additional display of some stats bonuses, regeneration, xp reward for killing, etc.
		detailedUnitDescription = true,

		-- Additional display of some stats bonuses, drain, critical hit, custom attack ratios, etc.
		detailedAttackDescription = true,

		-- Additional display of dynamic upgrade values (only for unit type encyclopedia to avoid clutter)
		-- Enable detailedUnitDescription and/or detailedAttackDescription to show upgrade values for corresponding stats
		displayDynamicUpgradeValues = false,

		-- Additional display of bonus hit points
		-- Requires detailedUnitDescription
		displayBonusHp = false,

		-- Additional display of experience points reduction
		-- Requires detailedUnitDescription
		displayBonusXp = false,

		-- Display infinite effect indicator along with attack name (alternative to effect duration)
		-- Requires detailedUnitDescription
		displayInfiniteAttackIndicator = false,

		-- Display Critical Hit text in Attack section instead of Damage and Power sections
		-- Requires detailedAttackDescription
		displayCriticalHitTextInAttackName = false,

		-- Allows to update encyclopedia content on the fly by pressing specified keys.
		-- Used in combination with isShift/Ctrl/AltKeyPressed from unitEncyclopedia.lua
		updateOnShiftKeyPress = false,
		updateOnCtrlKeyPress = false,
		updateOnAltKeyPress = false,
	},
}
