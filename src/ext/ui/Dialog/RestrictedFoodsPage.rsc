// Dialog/RestrictedFoodsPage.rsc -- ext.restricted_foods's page in the extension window (NOT RETAIL; authored for this port, src/ext).
//
// docs/extensions/handins/RESTRICTEDFOODS.md, src/ext/restricted_foods/toolbar_page.cpp. One cell per edible food of the running
// town, a check box each; a checked food is not eaten by citizens, from the moment it is checked. The window around it is
// Dialog/ExtensionsWindow.rsc (one tab per active extension page); this file is only the page.
//
// The list is laid out like the Town Hall's Inventory tab (TownHall.rsc:inventoryTable / storageItem): a table of four
// columns of small cells, each a check box and a compact label (20 px icon, small font), so a modded game's hundreds of
// foods are a short scroll. The controller sizes the table and clones foodCell into it, as TownHallUI::InitInventory does.
//
// Controller: src/ext/restricted_foods/toolbar_page.cpp. It looks up, by name: tableRestrictedFoods, buttonRestrictedFoodsRow (the filter
// button is its one child), and in each cell checkRestrictedFoods and labelRestrictedFoods.
//
// Texts: Dialog/ExtensionsStringTable.rsc (the RestrictedFoods* keys).

// The page: the hint, the list, the filter button; 4 px between them.
RibbonDescription pageRestrictedFoods
{
	bool _vertical = true; int _cellPad = 4;
	ElementDescription _elements [ "textRestrictedFoodsHint" "foodList" "buttonRestrictedFoodsRow" ]
}

// The note under the tab: what the check boxes do.
TextDescription textRestrictedFoodsHint : "Extensions.rsc:textProfile"
{
	String _text = "RestrictedFoodsHint";
}

// The filter button (foods the town has had <-> all foods), under the list.
RibbonDescription buttonRestrictedFoodsRow
{
	Alignment _alignment = MidLeft; int _cellPad = 4;
	ElementDescription _elements [ "buttonRestrictedFoodsFilter" ]
}

// Derived from buttonOk / buttonOkText, not from borderButton / commonButtonText: the compiled game only holds the
// blocks something refers to as an element, so a block used only as a base (borderButton, commonButtonText) does not
// load by name ("the base ... did not load", live 2026-10-19).
ButtonDescription buttonRestrictedFoodsFilter : "SharedElements.rsc:buttonOk" { ElementDescription _content = "textRestrictedFoodsFilter"; }

TextDescription textRestrictedFoodsFilter : "SharedElements.rsc:buttonOkText"
{
	StringTable _stringTable = "ExtensionsStringTable.rsc"; String _text = "RestrictedFoodsFilterTown";
}

// The list frame: the Extensions dialog's (by reference) around the scrolling box of foods.
RibbonDescription foodList : "Extensions.rsc:entryList"
{
	ElementDescription _elements [ "foodBox" ]
}

// The scrolling box: the Extensions list's container (scroll bar, size) with the table as its content.
ContainerDescription foodBox : "Extensions.rsc:tableList" { ElementDescription _content = "tableRestrictedFoods"; }

// Four columns (the Town Hall's has three, wider, cells). The controller sizes it (rows = foods / 4) and fills it with foodCell.
TableDescription tableRestrictedFoods
{
	int _width = 4;
	int _height = 1;
	TableItem _items [ { int _x = -1; int _y = -1; ElementDescription _element = "foodCell"; } ]
}

// One food: the check box and the compact label. The border is the Mods list's row border, tinted red by the controller
// while the food is not eaten.
RibbonDescription foodCell
{
	ElementDescription _border = "Mod.rsc:borderLine";
	int _leftPad = 1; int _topPad = 1; int _bottomPad = 1; int _cellPad = 2;
	int _minWidth = 140;		// four of these fill the list's width
	ElementDescription _elements [ "checkRestrictedFoods" "labelRestrictedFoods" ]
}

CheckDescription checkRestrictedFoods : "Extensions.rsc:checkLoaded"
{
	StringTable _toolTipStringTable = "ExtensionsStringTable.rsc"; String _toolTipText = "RestrictedFoodsCheckTip";
}

// The Town Hall's inventory item label (20 px icon, small font, 132 px wide), its text from our table's "@0".
LabelDescription labelRestrictedFoods : "TownHall.rsc:storageItem"
{
	StringTable _stringTable = "ExtensionsStringTable.rsc"; String _text = "RestrictedFoodsParam";
}
