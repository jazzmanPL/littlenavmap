/*****************************************************************************
* Copyright 2015-2020 Alexander Barthel alex@littlenavmap.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "routecontroller.h"
#include "routestring/routestringwriter.h"
#include "routestring/routestringreader.h"
#include "routestring/routestringdialog.h"

#include "navapp.h"
#include "atools.h"
#include "options/optiondata.h"
#include "gui/helphandler.h"
#include "query/procedurequery.h"
#include "common/constants.h"
#include "common/tabindexes.h"
#include "fs/db/databasemeta.h"
#include "common/formatter.h"
#include "fs/perf/aircraftperf.h"
#include "search/proceduresearch.h"
#include "common/unit.h"
#include "exception.h"
#include "export/csvexporter.h"
#include "gui/actiontextsaver.h"
#include "gui/actionstatesaver.h"
#include "gui/errorhandler.h"
#include "track/trackcontroller.h"
#include "gui/itemviewzoomhandler.h"
#include "gui/widgetstate.h"
#include "query/mapquery.h"
#include "query/airwaytrackquery.h"
#include "query/airportquery.h"
#include "mapgui/mapwidget.h"
#include "parkingdialog.h"
#include "routing/routefinder.h"
#include "routing/routenetwork.h"
#include "route/customproceduredialog.h"
#include "settings/settings.h"
#include "routeextractor.h"
#include "ui_mainwindow.h"
#include "gui/dialog.h"
#include "route/routealtitude.h"
#include "routeexport/routeexport.h"
#include "route/userwaypointdialog.h"
#include "route/flightplanentrybuilder.h"
#include "fs/pln/flightplanio.h"
#include "routestring/routestringdialog.h"
#include "util/htmlbuilder.h"
#include "mapgui/mapmarkhandler.h"
#include "common/symbolpainter.h"
#include "common/mapcolors.h"
#include "common/unit.h"
#include "route/routecalcwindow.h"
#include "common/unitstringtool.h"
#include "perf/aircraftperfcontroller.h"
#include "fs/sc/simconnectdata.h"
#include "gui/tabwidgethandler.h"
#include "gui/choicedialog.h"
#include "geo/calculations.h"
#include "routing/routenetworkloader.h"
#include "fs/util/fsutil.h"

#include <QClipboard>
#include <QFile>
#include <QStandardItemModel>
#include <QInputDialog>
#include <QFileInfo>
#include <QTextTable>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QScrollBar>

namespace rcol {
// Route table column indexes
enum RouteColumns
{
  FIRST_COLUMN,
  IDENT = FIRST_COLUMN,
  REGION,
  NAME,
  PROCEDURE,
  AIRWAY_OR_LEGTYPE,
  RESTRICTION,
  TYPE,
  FREQ,
  RANGE,
  COURSE,
  COURSETRUE,
  DIST,
  REMAINING_DISTANCE,
  LEG_TIME,
  ETA,
  FUEL_WEIGHT,
  FUEL_VOLUME,
  WIND,
  WIND_HEAD_TAIL,
  ALTITUDE,
  LATITUDE,
  LONGITUDE,
  REMARKS,
  LAST_COLUMN = REMARKS
};

}

/* Maximum lines in flight plan and waypoint remarks for printing and HTML export */
const static int MAX_REMARK_LINES_HTML_AND_PRINT = 1000;
const static int MAX_REMARK_COLS_HTML_AND_PRINT = 200;

using atools::fs::pln::Flightplan;
using atools::fs::pln::FlightplanEntry;
using namespace atools::geo;

namespace pln = atools::fs::pln;

RouteController::RouteController(QMainWindow *parentWindow, QTableView *tableView)
  : QObject(parentWindow), mainWindow(parentWindow), view(tableView)
{
  mapQuery = NavApp::getMapQuery();
  airportQuery = NavApp::getAirportQuerySim();
  airwayQuery = NavApp::getAirwayTrackQuery();

  routeColumns = QList<QString>({QObject::tr("Ident"),
                                 QObject::tr("Region"),
                                 QObject::tr("Name"),
                                 QObject::tr("Procedure"),
                                 QObject::tr("Airway or\nProcedure"),
                                 QObject::tr("Restriction\n%alt%/%speed%"),
                                 QObject::tr("Type"),
                                 QObject::tr("Freq.\nMHz/kHz/Cha."),
                                 QObject::tr("Range\n%dist%"),
                                 QObject::tr("Course\n°M"),
                                 QObject::tr("Course\n°T"),
                                 QObject::tr("Distance\n%dist%"),
                                 QObject::tr("Remaining\n%dist%"),
                                 QObject::tr("Leg Time\nhh:mm"),
                                 QObject::tr("ETA\nhh:mm"),
                                 QObject::tr("Fuel Rem.\n%weight%"),
                                 QObject::tr("Fuel Rem.\n%volume%"),
                                 QObject::tr("Wind\n°M/%speed%"),
                                 QObject::tr("Head- or Tailwind\n%speed%"),
                                 QObject::tr("Altitude\n%alt%"),
                                 QObject::tr("Latitude"),
                                 QObject::tr("Longitude"),
                                 QObject::tr("Remarks")});

  routeColumnTooltips = QList<QString>(
  {
    QObject::tr("ICAO ident of the navaid or airport."),
    QObject::tr("Two letter region code of a navaid."),
    QObject::tr("Name of airport or radio navaid."),
    QObject::tr("Either SID, SID transition, STAR, STAR transition, transition, "
                "approach or missed plus the name of the procedure."),
    QObject::tr("Contains the airway name for en route legs or procedure instruction."),
    QObject::tr("Either minimum altitude for en route airway segment, "
                "procedure altitude restriction or procedure speed limit."),
    QObject::tr("Type of a radio navaid. Shows ILS or LOC for\n"
                "localizer approaches on the last runway leg."),
    QObject::tr("Frequency or channel of a radio navaid.\n"
                "Also shows ILS or localizer frequency for corresponding approaches on the last runway leg."),
    QObject::tr("Range of a radio navaid if available."),
    QObject::tr("Magnetic start course of the great circle route connecting the two waypoints of the leg."),
    QObject::tr("True start course of the great circle route connecting the two waypoints of the leg."),
    QObject::tr("Distance of the flight plan leg."),
    QObject::tr("Remaining distance to destination airport or procedure end point."),
    QObject::tr("Flying time for this leg.\nCalculated based on the selected aircraft performance profile."),
    QObject::tr("Estimated time of arrival.\nCalculated based on the selected aircraft performance profile."),
    QObject::tr("Fuel weight remaining at waypoint, once for volume and once for weight.\n"
                "Calculated based on the aircraft performance profile."),
    QObject::tr("Fuel volume remaining at waypoint, once for volume and once for weight.\n"
                "Calculated based on the aircraft performance profile."),
    QObject::tr("Wind direction and speed at waypoint."),
    QObject::tr("Head- or tailwind at waypoint."),
    QObject::tr("Altitude at waypoint\n"
                "Calculated based on the aircraft performance profile."),
    QObject::tr("Waypoint latitude in format as selected in options."),
    QObject::tr("Waypoint longitude in format as selected in options."),
    QObject::tr("Turn instructions, flyover or related navaid for procedure legs.")
  });

  flightplanIO = new atools::fs::pln::FlightplanIO();

  Ui::MainWindow *ui = NavApp::getMainUi();
  tabHandlerRoute = new atools::gui::TabWidgetHandler(ui->tabWidgetRoute,
                                                      QIcon(":/littlenavmap/resources/icons/tabbutton.svg"),
                                                      tr("Open or close tabs"));
  tabHandlerRoute->init(rc::TabRouteIds, lnm::ROUTEWINDOW_WIDGET_TABS);

  // Update units
  units = new UnitStringTool();
  units->init({
    ui->spinBoxRouteAlt,
    ui->spinBoxAircraftPerformanceWindSpeed
  });

  ui->labelRouteError->setVisible(false);

  // Set default table cell and font size to avoid Qt overly large cell sizes
  zoomHandler = new atools::gui::ItemViewZoomHandler(view);
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
  connect(NavApp::navAppInstance(), &atools::gui::Application::fontChanged, this, &RouteController::fontChanged);
#endif

  entryBuilder = new FlightplanEntryBuilder();

  symbolPainter = new SymbolPainter();

  // Use saved font size for table view
  zoomHandler->zoomPercent(OptionData::instance().getGuiRouteTableTextSize());

  view->setContextMenuPolicy(Qt::CustomContextMenu);

  // Create flight plan calculation caches
  routeNetworkRadio = new atools::routing::RouteNetwork(atools::routing::SOURCE_RADIO);
  routeNetworkAirway = new atools::routing::RouteNetwork(atools::routing::SOURCE_AIRWAY);

  routeWindow = new RouteCalcWindow(mainWindow);

  // Set up undo/redo framework
  undoStack = new QUndoStack(mainWindow);
  undoStack->setUndoLimit(ROUTE_UNDO_LIMIT);

  QAction *undoAction = undoStack->createUndoAction(mainWindow, tr("&Undo Flight Plan"));
  undoAction->setIcon(QIcon(":/littlenavmap/resources/icons/undo.svg"));
  undoAction->setShortcut(QKeySequence("Ctrl+Z"));

  QAction *redoAction = undoStack->createRedoAction(mainWindow, tr("&Redo Flight Plan"));
  redoAction->setIcon(QIcon(":/littlenavmap/resources/icons/redo.svg"));
  redoAction->setShortcut(QKeySequence("Ctrl+Y"));

  connect(redoAction, &QAction::triggered, this, &RouteController::redoTriggered);
  connect(undoAction, &QAction::triggered, this, &RouteController::undoTriggered);

  ui->toolBarRoute->insertAction(ui->actionRouteSelectParking, undoAction);
  ui->toolBarRoute->insertAction(ui->actionRouteSelectParking, redoAction);

  ui->menuRoute->insertActions(ui->actionRouteSelectParking, {undoAction, redoAction});
  ui->menuRoute->insertSeparator(ui->actionRouteSelectParking);

  connect(ui->spinBoxRouteAlt, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
          this, &RouteController::routeAltChanged);
  connect(ui->comboBoxRouteType, static_cast<void (QComboBox::*)(int)>(&QComboBox::activated),
          this, &RouteController::routeTypeChanged);

  connect(view, &QTableView::doubleClicked, this, &RouteController::doubleClick);
  connect(view, &QTableView::customContextMenuRequested, this, &RouteController::tableContextMenu);
  connect(this, &RouteController::routeChanged, this, &RouteController::updateRemarkWidget);
  connect(ui->plainTextEditRouteRemarks, &QPlainTextEdit::textChanged, this, &RouteController::remarksTextChanged);

  // Update route altitude and elevation profile with a delay
  connect(&routeAltDelayTimer, &QTimer::timeout, this, &RouteController::routeAltChangedDelayed);
  routeAltDelayTimer.setSingleShot(true);

  // Clear selection after inactivity
  // Or move active to top after inactivity (no scrolling)
  connect(&tableCleanupTimer, &QTimer::timeout, this, &RouteController::cleanupTableTimeout);
  tableCleanupTimer.setInterval(OptionData::instance().getSimCleanupTableTime() * 1000);
  tableCleanupTimer.setSingleShot(true);

  // Restart timer when scrolling or holding the slider
  connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, &RouteController::viewScrolled);
  connect(view->horizontalScrollBar(), &QScrollBar::valueChanged, this, &RouteController::viewScrolled);
  connect(view->verticalScrollBar(), &QScrollBar::sliderPressed, this, &RouteController::sliderPressedOrReleased);
  connect(view->horizontalScrollBar(), &QScrollBar::sliderPressed, this, &RouteController::sliderPressedOrReleased);
  connect(view->verticalScrollBar(), &QScrollBar::sliderReleased, this, &RouteController::sliderPressedOrReleased);
  connect(view->horizontalScrollBar(), &QScrollBar::sliderReleased, this, &RouteController::sliderPressedOrReleased);

  // set up table view
  view->horizontalHeader()->setSectionsMovable(true);
  view->verticalHeader()->setSectionsMovable(false);
  view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);

  model = new QStandardItemModel();
  QItemSelectionModel *m = view->selectionModel();
  view->setModel(model);
  delete m;

  // Avoid stealing of keys from other default menus
  ui->actionRouteLegDown->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteLegUp->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteDeleteLeg->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteShowInformation->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteShowApproaches->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteShowApproachesCustom->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteShowOnMap->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteTableSelectNothing->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteTableSelectAll->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteActivateLeg->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteSetMark->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteResetView->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteTableCopy->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionRouteEditUserWaypoint->setShortcutContext(Qt::WidgetWithChildrenShortcut);

  // Add action/shortcuts to table view
  view->addActions({ui->actionRouteLegDown, ui->actionRouteLegUp, ui->actionRouteDeleteLeg,
                    ui->actionRouteTableCopy, ui->actionRouteShowInformation,
                    ui->actionRouteShowApproaches, ui->actionRouteShowApproachesCustom,
                    ui->actionRouteShowOnMap, ui->actionRouteTableSelectNothing, ui->actionRouteTableSelectAll,
                    ui->actionRouteActivateLeg, ui->actionRouteResetView, ui->actionRouteSetMark,
                    ui->actionRouteEditUserWaypoint});

  void (RouteController::*selChangedPtr)(const QItemSelection& selected, const QItemSelection& deselected) =
    &RouteController::tableSelectionChanged;

  if(view->selectionModel() != nullptr)
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, selChangedPtr);

  // Connect actions - actions without shortcut key are used in the context menu method directly
  connect(ui->actionRouteTableCopy, &QAction::triggered, this, &RouteController::tableCopyClipboard);
  connect(ui->actionRouteLegDown, &QAction::triggered, this, &RouteController::moveSelectedLegsDown);
  connect(ui->actionRouteLegUp, &QAction::triggered, this, &RouteController::moveSelectedLegsUp);
  connect(ui->actionRouteDeleteLeg, &QAction::triggered, this, &RouteController::deleteSelectedLegs);
  connect(ui->actionRouteEditUserWaypoint, &QAction::triggered, this, &RouteController::editUserWaypointTriggered);

  connect(ui->actionRouteShowInformation, &QAction::triggered, this, &RouteController::showInformationMenu);
  connect(ui->actionRouteShowApproaches, &QAction::triggered, this, &RouteController::showProceduresMenu);
  connect(ui->actionRouteShowApproachesCustom, &QAction::triggered, this, &RouteController::showProceduresMenuCustom);
  connect(ui->actionRouteShowOnMap, &QAction::triggered, this, &RouteController::showOnMapMenu);

  connect(ui->dockWidgetRoute, &QDockWidget::visibilityChanged, this, &RouteController::dockVisibilityChanged);
  connect(ui->actionRouteTableSelectNothing, &QAction::triggered, this, &RouteController::clearTableSelection);
  connect(ui->actionRouteTableSelectAll, &QAction::triggered, this, &RouteController::selectAllTriggered);
  connect(ui->pushButtonRouteClearSelection, &QPushButton::clicked, this, &RouteController::clearTableSelection);
  connect(ui->pushButtonRouteHelp, &QPushButton::clicked, this, &RouteController::helpClicked);
  connect(ui->actionRouteActivateLeg, &QAction::triggered, this, &RouteController::activateLegTriggered);
  connect(ui->actionRouteVisibleColumns, &QAction::triggered, this, &RouteController::visibleColumnsTriggered);
  connect(ui->pushButtonRouteSettings, &QPushButton::clicked, this, &RouteController::visibleColumnsTriggered);

  connect(this, &RouteController::routeChanged, routeWindow, &RouteCalcWindow::routeChanged);
  connect(routeWindow, &RouteCalcWindow::calculateClicked, this, &RouteController::calculateRoute);
  connect(routeWindow, &RouteCalcWindow::calculateDirectClicked, this, &RouteController::calculateDirect);
  connect(routeWindow, &RouteCalcWindow::calculateReverseClicked, this, &RouteController::reverseRoute);
  connect(routeWindow, &RouteCalcWindow::downloadTrackClicked,
          NavApp::getTrackController(), &TrackController::startDownload);

  connect(ui->labelRouteInfo, &QLabel::linkActivated, this, &RouteController::flightplanLabelLinkActivated);

  // UI editor cannot deal with line breaks - set text here
  ui->textBrowserViewRoute->setPlaceholderText(
    tr("No flight plan.\n\n"
       "Right-click the center of an airport symbol on the map and select it as departure/destination from the context menu, "
       "or use the airport search (press \"F4\") to select departure/destination from the context menu of the search result table."));
  updatePlaceholderWidget();
}

RouteController::~RouteController()
{
  routeAltDelayTimer.stop();
  delete routeWindow;
  delete tabHandlerRoute;
  delete units;
  delete entryBuilder;
  delete model;
  delete undoStack;
  delete routeNetworkRadio;
  delete routeNetworkAirway;
  delete zoomHandler;
  delete symbolPainter;
  delete flightplanIO;
}

void RouteController::fontChanged()
{
  qDebug() << Q_FUNC_INFO;

  zoomHandler->fontChanged();
  optionsChanged();
}

void RouteController::undoTriggered()
{
  NavApp::setStatusMessage(QString(tr("Undo flight plan change.")));
}

void RouteController::redoTriggered()
{
  NavApp::setStatusMessage(QString(tr("Redo flight plan change.")));
}

/* Ctrl-C - copy selected table contents in CSV format to clipboard */
void RouteController::tableCopyClipboard()
{
  qDebug() << "RouteController::tableCopyClipboard";

  const Route& rt = route;
  const QStandardItemModel *mdl = model;

  // Use callback to get data to avoid truncated remarks
  auto dataFunc = [&rt, &mdl](int row, int column) -> QVariant {
                    if(column == rcol::REMARKS)
                      // Full remarks
                      return rt.value(row).getComment();
                    else
                      // Formatted view content
                      return mdl->data(mdl->index(row, column));
                  };

  QString csv;
  int exported = CsvExporter::selectionAsCsv(view, true /* rows */, true /* header */, csv,
                                             {}, nullptr, dataFunc);

  if(!csv.isEmpty())
  {
    QApplication::clipboard()->setText(csv);
    NavApp::setStatusMessage(QString(tr("Copied %1 entries as CSV to clipboard.")).arg(exported));
  }
}

void RouteController::flightplanTableAsTextTable(QTextCursor& cursor, const QBitArray& selectedCols,
                                                 float fontPointSize) const
{
  // Check if model is already initailized
  if(model->rowCount() == 0)
    return;

  int numCols = selectedCols.count(true);

  // Prepare table format ===================================
  QTextTableFormat fmt;
  fmt.setHeaderRowCount(1);
  fmt.setCellPadding(1);
  fmt.setCellSpacing(0);
  fmt.setBorder(2);
  fmt.setBorderBrush(Qt::lightGray);
  fmt.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
  QTextTable *table = cursor.insertTable(model->rowCount() + 1, numCols, fmt);

  // Cell alignment formats ===================================
  QTextBlockFormat alignRight;
  alignRight.setAlignment(Qt::AlignRight);
  QTextBlockFormat alignLeft;
  alignLeft.setAlignment(Qt::AlignLeft);

  // Text size and alternating background formats ===================================
  QTextCharFormat altFormat1 = table->cellAt(0, 0).format();
  altFormat1.setFontPointSize(fontPointSize);
  altFormat1.setBackground(mapcolors::mapPrintRowColor);

  QTextCharFormat altFormat2 = altFormat1;
  altFormat2.setBackground(mapcolors::mapPrintRowColorAlt);

  // Header font and background ================
  QTextCharFormat headerFormat = altFormat1;
  headerFormat.setFontWeight(QFont::Bold);
  headerFormat.setBackground(mapcolors::mapPrintHeaderColor);

  // Fill header =====================================================================
  // Table header from GUI widget
  QHeaderView *header = view->horizontalHeader();

  int cellIdx = 0;
  for(int viewCol = 0; viewCol < model->columnCount(); viewCol++)
  {
    // Convert view position to model position - needed to keep order
    int logicalCol = header->logicalIndex(viewCol);

    if(logicalCol == -1)
      continue;

    if(!selectedCols.at(logicalCol))
      // Ignore if not selected in the print dialog
      continue;

    table->cellAt(0, cellIdx).setFormat(headerFormat);
    cursor.setPosition(table->cellAt(0, cellIdx).firstPosition());

    QString txt = model->headerData(logicalCol, Qt::Horizontal).toString().replace("-\n", "").replace("\n", " ");
    cursor.insertText(txt);

    cellIdx++;
  }

  // Fill table =====================================================================
  for(int row = 0; row < model->rowCount(); row++)
  {
    cellIdx = 0;
    for(int viewCol = 0; viewCol < model->columnCount(); viewCol++)
    {
      int logicalCol = header->logicalIndex(viewCol);
      if(logicalCol == -1)
        continue;

      if(!selectedCols.at(logicalCol))
        // Ignore if not selected in the print dialog
        continue;

      const QStandardItem *item = model->item(row, logicalCol);

      if(item != nullptr)
      {
        // Alternating background =============================
        QTextCharFormat textFormat = (row % 2) == 0 ? altFormat1 : altFormat2;

        // Determine font color base on leg =============
        const RouteLeg& leg = route.value(row);
        if(leg.isAlternate())
          textFormat.setForeground(mapcolors::routeAlternateTableColor);
        else if(leg.isAnyProcedure())
          textFormat.setForeground(leg.getProcedureLeg().isMissed() ?
                                   mapcolors::routeProcedureMissedTableColor :
                                   mapcolors::routeProcedureTableColor);
        else if((logicalCol == rcol::IDENT && leg.getMapObjectType() == map::INVALID) ||
                (logicalCol == rcol::AIRWAY_OR_LEGTYPE && leg.isRoute() &&
                 leg.isAirwaySetAndInvalid(route.getCruisingAltitudeFeet())))
          textFormat.setForeground(Qt::red);
        else
          textFormat.setForeground(Qt::black);

        if(logicalCol == rcol::IDENT)
          // Make ident bold
          textFormat.setFontWeight(QFont::Bold);

        if(logicalCol == rcol::REMARKS)
          // Make remarks small
          textFormat.setFontPointSize(textFormat.fontPointSize() * 0.8);

        table->cellAt(row + 1, cellIdx).setFormat(textFormat);
        cursor.setPosition(table->cellAt(row + 1, cellIdx).firstPosition());

        // Assign alignment to cell
        if(item->textAlignment() == Qt::AlignRight)
          cursor.setBlockFormat(alignRight);
        else
          cursor.setBlockFormat(alignLeft);

        if(logicalCol == rcol::REMARKS)
          cursor.insertText(atools::elideTextLinesShort(leg.getComment(), MAX_REMARK_LINES_HTML_AND_PRINT,
                                                        MAX_REMARK_COLS_HTML_AND_PRINT, true, true));
        else
          cursor.insertText(item->text());
      }
      cellIdx++;
    }
  }

  // Move cursor after table
  cursor.setPosition(table->lastPosition() + 1);
}

void RouteController::flightplanHeaderPrint(atools::util::HtmlBuilder& html, bool titleOnly) const
{
  html.text(buildFlightplanLabel(true /* print */, false /* widget */, titleOnly), atools::util::html::NO_ENTITIES);

  if(!titleOnly)
    html.p(buildFlightplanLabel2(true /* print */), atools::util::html::NO_ENTITIES | atools::util::html::BIG);

  if(!route.getFlightplan().getComment().isEmpty())
  {
    html.p().b(tr("Flight Plan Remarks")).pEnd();
    html.table(1, 2, 0, 0, html.getRowBackColor());
    html.tr().td().text(atools::elideTextLinesShort(route.getFlightplan().getComment(), MAX_REMARK_LINES_HTML_AND_PRINT,
                                                    MAX_REMARK_COLS_HTML_AND_PRINT, true, true),
                        atools::util::html::SMALL).tdEnd().trEnd();
    html.tableEnd().br();
  }
}

QString RouteController::getFlightplanTableAsHtmlDoc(float iconSizePixel) const
{
  // Embedded style sheet =================================
  QString css("table { border-collapse: collapse; } "
              "th, td { border-right: 1px solid #aaa; padding: 0px 3px 0px 3px; white-space: nowrap; font-size: 0.9em; } "
              "th { white-space: normal; padding: 3px 3px 3px 3px; font-size: 0.95em; } "
              "tr:hover {background-color: #c8c8c8; } ");

  // Metadata in header =================================
  QStringList headerLines({"<meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\" />",
                           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />"});

  atools::util::HtmlBuilder html(true);
  html.doc(tr("%1 - %2").arg(QApplication::applicationName()).arg(QFileInfo(routeFilename).fileName()),
           css, QString() /* bodyStyle */, headerLines);
  html.text(NavApp::getRouteController()->getFlightplanTableAsHtml(iconSizePixel, true /* print */),
            atools::util::html::NO_ENTITIES);

  // Add footer ==============================
  html.p().small(tr("%1 Version %2 (revision %3) on %4 ").
                 arg(QApplication::applicationName()).
                 arg(QApplication::applicationVersion()).
                 arg(GIT_REVISION).
                 arg(QLocale().toString(QDateTime::currentDateTime()))).pEnd();
  html.docEnd();
  return html.getHtml();
}

QString RouteController::getFlightplanTableAsHtml(float iconSizePixel, bool print) const
{
  qDebug() << Q_FUNC_INFO;

  // Check if model is already initailized
  if(model->rowCount() == 0)
    return QString();

  using atools::util::HtmlBuilder;

  atools::util::HtmlBuilder html(mapcolors::webTableBackgroundColor, mapcolors::webTableAltBackgroundColor);
  int minColWidth = view->horizontalHeader()->minimumSectionSize() + 1;

  // Header lines
  html.p(buildFlightplanLabel(print), atools::util::html::NO_ENTITIES | atools::util::html::BIG);
  html.p(buildFlightplanLabel2(print), atools::util::html::NO_ENTITIES | atools::util::html::BIG);

  if(print && !route.getFlightplan().getComment().isEmpty())
  {
    html.p().b(tr("Flight Plan Remarks")).pEnd();
    html.table(1, 2, 0, 0, html.getRowBackColor());
    html.tr().td().text(atools::elideTextLinesShort(route.getFlightplan().getComment(), MAX_REMARK_LINES_HTML_AND_PRINT,
                                                    MAX_REMARK_COLS_HTML_AND_PRINT, true, true),
                        atools::util::html::SMALL).tdEnd().trEnd();
    html.tableEnd().br();
  }

  html.table();

  // Table header
  QHeaderView *header = view->horizontalHeader();
  html.tr(Qt::lightGray);
  html.th(QString()); // Icon
  for(int viewCol = 0; viewCol < model->columnCount(); viewCol++)
  {
    // Convert view position to model position - needed to keep order
    int logicalCol = header->logicalIndex(viewCol);

    if(logicalCol == -1)
      continue;

    if(!view->isColumnHidden(logicalCol) && view->columnWidth(logicalCol) > minColWidth)
      html.th(model->headerData(logicalCol, Qt::Horizontal).
              toString().replace("-\n", "-<br/>").replace("\n", "<br/>"), atools::util::html::NO_ENTITIES);
  }
  html.trEnd();

  int nearestLegIndex = route.getActiveLegIndexCorrected();

  // Table content
  for(int row = 0; row < model->rowCount(); row++)
  {
    // First column is icon
    html.tr(nearestLegIndex != row ? QColor() : mapcolors::nextWaypointColor);
    const RouteLeg& leg = route.value(row);

    if(iconSizePixel > 0.f)
    {
      int sizeInt = atools::roundToInt(iconSizePixel);

      html.td();
      html.img(iconForLeg(leg, atools::roundToInt(iconSizePixel)), QString(), QString(), QSize(sizeInt, sizeInt));
      html.tdEnd();
    }

    // Rest of columns
    for(int viewCol = 0; viewCol < model->columnCount(); viewCol++)
    {
      int logicalCol = header->logicalIndex(viewCol);
      if(logicalCol == -1)
        continue;

      if(!view->isColumnHidden(logicalCol) && view->columnWidth(logicalCol) > minColWidth)
      {
        QStandardItem *item = model->item(row, logicalCol);

        if(item != nullptr)
        {
          QColor color(Qt::black);
          if(leg.isAlternate())
            color = mapcolors::routeAlternateTableColor;
          else if(leg.isAnyProcedure())
            color = leg.getProcedureLeg().isMissed() ?
                    mapcolors::routeProcedureMissedTableColor :
                    mapcolors::routeProcedureTableColor;
          else if((logicalCol == rcol::IDENT && leg.getMapObjectType() == map::INVALID) ||
                  (logicalCol == rcol::AIRWAY_OR_LEGTYPE && leg.isRoute() &&
                   leg.isAirwaySetAndInvalid(route.getCruisingAltitudeFeet())))
            color = Qt::red;

          atools::util::html::Flags flags = atools::util::html::NONE;
          // Make ident column text bold
          if(logicalCol == rcol::IDENT)
            flags |= atools::util::html::BOLD;

          // Remarks in small text
          if(logicalCol == rcol::REMARKS)
            flags |= atools::util::html::SMALL;

          // Take over alignment from model
          if(item->textAlignment().testFlag(Qt::AlignRight))
            flags |= atools::util::html::ALIGN_RIGHT;

          if(logicalCol == rcol::REMARKS)
            html.td(atools::elideTextLinesShort(leg.getComment(), MAX_REMARK_LINES_HTML_AND_PRINT,
                                                MAX_REMARK_COLS_HTML_AND_PRINT, true, true), flags, color);
          else
            html.td(item->text(), flags, color);
        }
        else
          html.td(QString());
      }
    }
    html.trEnd();
  }
  html.tableEnd();
  return html.getHtml();
}

void RouteController::routeStringToClipboard() const
{
  qDebug() << Q_FUNC_INFO;

  QString str = RouteStringWriter().createStringForRoute(route,
                                                         NavApp::getRouteCruiseSpeedKts(),
                                                         RouteStringDialog::getOptionsFromSettings());

  qDebug() << "route string" << str;
  if(!str.isEmpty())
    QApplication::clipboard()->setText(str);

  NavApp::setStatusMessage(QString(tr("Flight plan string to clipboard.")));
}

void RouteController::aircraftPerformanceChanged()
{
  qDebug() << Q_FUNC_INFO;
  if(!route.isEmpty())
  {
    // Get type, speed and cruise altitude from widgets
    updateTableHeaders(); // Update lbs/gal for fuel
    updateFlightplanFromWidgets();
    route.updateLegAltitudes();

    updateModelTimeFuelWind();
    updateModelHighlights();
    highlightNextWaypoint(route.getActiveLegIndexCorrected());
  }
  updateWindowLabel();

  // Emit also for empty route to catch performance changes
  emit routeChanged(true);
}

void RouteController::windUpdated()
{
  qDebug() << Q_FUNC_INFO;
  if(!route.isEmpty())
  {
    // Get type, speed and cruise altitude from widgets
    route.updateLegAltitudes();

    updateModelTimeFuelWind();
    updateModelHighlights();
    highlightNextWaypoint(route.getActiveLegIndexCorrected());
  }
  updateWindowLabel();

  // Emit also for empty route to catch performance changes
  emit routeChanged(false);
  // emit routeChanged(true);
}

/* Spin box altitude has changed value */
void RouteController::routeAltChanged()
{
  RouteCommand *undoCommand = nullptr;

  if(!route.isEmpty() /*&& route.getFlightplan().canSaveAltitude()*/)
    undoCommand = preChange(tr("Change Altitude"), rctype::ALTITUDE);

  // Get type, speed and cruise altitude from widgets
  updateFlightplanFromWidgets();

  postChange(undoCommand);

  updateWindowLabel();
  NavApp::updateWindowTitle();

  // Calls RouteController::routeAltChangedDelayed
  routeAltDelayTimer.start(ROUTE_ALT_CHANGE_DELAY_MS);
}

void RouteController::routeAltChangedDelayed()
{
  route.updateLegAltitudes();

  // Update performance
  updateModelTimeFuelWind();
  updateModelHighlights();
  highlightNextWaypoint(route.getActiveLegIndexCorrected());

  updateWindowLabel();

  // Delay change to avoid hanging spin box when profile updates
  emit routeAltitudeChanged(route.getCruisingAltitudeFeet());
}

/* Combo box route type has value changed */
void RouteController::routeTypeChanged()
{
  RouteCommand *undoCommand = nullptr;

  if(!route.isEmpty() /*&& route.getFlightplan().canSaveFlightplanType()*/)
    undoCommand = preChange(tr("Change Type"));

  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  postChange(undoCommand);

  NavApp::updateWindowTitle();

  if(!route.isEmpty())
  {
    emit routeChanged(false);
    Ui::MainWindow *ui = NavApp::getMainUi();
    NavApp::setStatusMessage(tr("Flight plan type changed to %1.").arg(ui->comboBoxRouteType->currentText()));
  }
}

bool RouteController::selectDepartureParking()
{
  qDebug() << Q_FUNC_INFO;

  const map::MapAirport& airport = route.getDepartureAirportLeg().getAirport();
  ParkingDialog dialog(mainWindow, airport);

  int result = dialog.exec();
  dialog.hide();

  if(result == QDialog::Accepted)
  {
    // Set either start of parking
    map::MapParking parking;
    map::MapStart start;
    if(dialog.getSelectedParking(parking))
    {
      routeSetParking(parking);
      return true;
    }
    else if(dialog.getSelectedStartPosition(start))
    {
      routeSetStartPosition(start);
      return true;
    }
  }
  return false;
}

void RouteController::saveState()
{
  Ui::MainWindow *ui = NavApp::getMainUi();

  atools::gui::WidgetState(lnm::ROUTE_VIEW).save({view, ui->comboBoxRouteType,
                                                  ui->spinBoxRouteAlt,
                                                  ui->actionRouteFollowSelection});

  atools::settings::Settings::instance().setValue(lnm::ROUTE_FILENAME, routeFilename);
  tabHandlerRoute->saveState();
  routeWindow->saveState();
}

void RouteController::updateTableHeaders()
{
  using atools::fs::perf::AircraftPerf;

  QList<QString> routeHeaders(routeColumns);

  for(QString& str : routeHeaders)
    str = Unit::replacePlaceholders(str);

  model->setHorizontalHeaderLabels(routeHeaders);
}

void RouteController::restoreState()
{
  tabHandlerRoute->restoreState();
  routeWindow->restoreState();
  Ui::MainWindow *ui = NavApp::getMainUi();
  updateTableHeaders();

  atools::gui::WidgetState state(lnm::ROUTE_VIEW, true, true);
  state.restore({view, ui->comboBoxRouteType, ui->spinBoxRouteAlt, ui->actionRouteFollowSelection});

  if(OptionData::instance().getFlags() & opts::STARTUP_LOAD_ROUTE)
  {
    QString newRouteFilename = atools::settings::Settings::instance().valueStr(lnm::ROUTE_FILENAME);

    if(!newRouteFilename.isEmpty())
    {
      if(QFile::exists(newRouteFilename))
      {
        if(!loadFlightplan(newRouteFilename))
        {
          // Cannot be loaded - clear current filename
          routeFilename.clear();
          fileDepartureIdent.clear();
          fileDestinationIdent.clear();
          fileIfrVfr = pln::VFR;
          fileCruiseAlt = 0.f;
          route.clear();
        }
      }
      else
      {
        routeFilename.clear();
        fileDepartureIdent.clear();
        fileDestinationIdent.clear();
        fileIfrVfr = pln::VFR;
        fileCruiseAlt = 0.f;
        route.clear();
      }
    }
  }

  if(route.isEmpty())
    updateFlightplanFromWidgets();

  units->update();

  connect(NavApp::getRouteTabHandler(), &atools::gui::TabWidgetHandler::tabOpened,
          this, &RouteController::updateRouteTabChangedStatus);
}

void RouteController::getSelectedRouteLegs(QList<int>& selLegIndexes) const
{
  if(NavApp::getMainUi()->dockWidgetRoute->isVisible())
  {
    if(view->selectionModel() != nullptr)
    {
      QItemSelection sm = view->selectionModel()->selection();
      for(const QItemSelectionRange& rng : sm)
      {
        for(int row = rng.top(); row <= rng.bottom(); ++row)
          selLegIndexes.append(row);
      }
    }
  }
}

void RouteController::newFlightplan()
{
  qDebug() << "newFlightplan";
  clearRoute();

  clearAllErrors();

  // Avoid warning when saving
  route.getFlightplan().setLnmFormat(true);

  // Copy current alt and type from widgets to flightplan
  updateFlightplanFromWidgets();

  route.createRouteLegsFromFlightplan();
  route.updateAll();
  route.updateLegAltitudes();
  route.updateRouteCycleMetadata();

  updateTableModel();
  updateMoveAndDeleteActions();
  remarksFlightPlanToWidget();

  emit routeChanged(true /* geometry changed */, true /* new flight plan */);
}

void RouteController::loadFlightplan(atools::fs::pln::Flightplan flightplan, atools::fs::pln::FileFormat format,
                                     const QString& filename, bool quiet, bool changed, bool adjustAltitude)
{
  qDebug() << Q_FUNC_INFO << filename;

#ifdef DEBUG_INFORMATION
  qDebug() << flightplan;
#endif

  clearAllErrors();

  if(format == atools::fs::pln::FLP)
  {
    // FLP is nothing more than a sort of route string
    // New waypoints along airways have to be inserted and waypoints have to be resolved without coordinate backup

    // Create a route string
    QStringList routeString;
    for(int i = 0; i < flightplan.getEntries().size(); i++)
    {
      const FlightplanEntry& entry = flightplan.at(i);
      if(!entry.getAirway().isEmpty())
        routeString.append(entry.getAirway());
      routeString.append(entry.getIdent());
    }
    qInfo() << "FLP generated route string" << routeString;

    // All is valid except the waypoint entries
    flightplan.getEntries().clear();

    // Use route string to overwrite the current incomplete flight plan object
    RouteStringReader rs(entryBuilder);
    rs.setPlaintextMessages(true);
    bool ok = rs.createRouteFromString(routeString.join(" "), rs::NONE, &flightplan);
    qInfo() << "createRouteFromString messages" << rs.getMessages();

    if(!ok)
    {
      atools::gui::Dialog::warning(mainWindow,
                                   tr("Loading of FLP flight plan failed:<br/><br/>") + rs.getMessages().join("<br/>"));
      return;

    }
    else if(!rs.getMessages().isEmpty())
      atools::gui::Dialog(mainWindow).showInfoMsgBox(lnm::ACTIONS_SHOW_LOAD_FLP_WARN,
                                                     tr("Warnings while loading FLP flight plan file:<br/><br/>") +
                                                     rs.getMessages().join("<br/>"),
                                                     tr("Do not &show this dialog again."));

    // Copy speed, type and altitude from GUI
    updateFlightplanFromWidgets(flightplan);
    adjustAltitude = true; // Change altitude based on airways later
  }
  else if(format == atools::fs::pln::FMS11 || format == atools::fs::pln::FMS3 || format == atools::fs::pln::FSC_PLN ||
          format == atools::fs::pln::FLIGHTGEAR || format == atools::fs::pln::GARMIN_FPL)
  {
    // Save altitude
    int cruiseAlt = flightplan.getCruisingAltitude();

    // Set type cruise altitude and speed since FMS and FSC do not support this
    updateFlightplanFromWidgets(flightplan);

    // Reset altitude after update from widgets
    if(cruiseAlt > 0)
      flightplan.setCruisingAltitude(cruiseAlt);
    else
      adjustAltitude = true; // Change altitude based on airways later
  }

  clearRoute();

  if(changed)
    undoIndexClean = -1;

  routeFilename = filename;

  // FSX and P3D use this field for helipad or runway numbers where the latter one is zero prefixed.
  // MSFS has no helipad number and we need a zero prefix added to the runway number to allow database queries
  if(format == atools::fs::pln::MSFS_PLN)
    flightplan.setDepartureParkingName(atools::fs::util::runwayNamePrefixZero(flightplan.getDepartureParkingName()));

  assignFlightplanPerfProperties(flightplan);
  route.setFlightplan(flightplan);

  route.createRouteLegsFromFlightplan();

  // test and error after undo/redo and switch

  loadProceduresFromFlightplan(false /* clear old procedure properties */);
  loadAlternateFromFlightplan();
  route.updateAll();
  route.updateAirwaysAndAltitude(adjustAltitude);

  // Save values for checking filename match when doing save
  fileDepartureIdent = route.getFlightplan().getDepartureIdent();
  fileDestinationIdent = route.getFlightplan().getDestinationIdent();
  fileIfrVfr = route.getFlightplan().getFlightplanType();
  fileCruiseAlt = route.getCruisingAltitudeFeet();

  route.updateLegAltitudes();
  route.updateRouteCycleMetadata();

  // Get number from user waypoint from user defined waypoint in fs flight plan
  entryBuilder->setCurUserpointNumber(route.getNextUserWaypointNumber());

  // Force update start position for other formats than FSX, P3D or LNMPLN since
  // these do not support loading and saving of start positions
  bool forceUpdate = format != atools::fs::pln::LNM_PLN && format != atools::fs::pln::FSX_PLN &&
                     format != atools::fs::pln::MSFS_PLN;

  // Do not create an entry on the undo stack since this plan file type does not support it
  bool showWarning = false;
  if(updateStartPositionBestRunway(forceUpdate /* force */, false /* undo */))
  {
    if(forceUpdate ? false : !quiet)
      showWarning = true;
  }

  remarksFlightPlanToWidget();
  updateTableModel();
  updateMoveAndDeleteActions();
  routeWindow->setCruisingAltitudeFt(route.getCruisingAltitudeFeet());

#ifdef DEBUG_INFORMATION
  qDebug() << Q_FUNC_INFO << route;
#endif

  emit routeChanged(true /* geometry changed */, true /* new flight plan */);

  if(showWarning)
  {
    NavApp::deleteSplashScreen();
    QTimer::singleShot(0, this, [ = ](void) -> void {
      atools::gui::Dialog(mainWindow).showInfoMsgBox(lnm::ACTIONS_SHOWROUTE_START_CHANGED,
                                                     tr("The flight plan had no valid start position.\n"
                                                        "The start position is now set to the longest "
                                                        "primary runway of the departure airport."),
                                                     tr("Do not &show this dialog again."));
    });
  }
}

/* Appends alternates to the end of the flight plan */
void RouteController::loadAlternateFromFlightplan()
{
  if(route.isEmpty())
    return;

  atools::fs::pln::Flightplan& fp = route.getFlightplan();
  QHash<QString, QString>& props = fp.getProperties();

  QStringList alternates = props.value(pln::ALTERNATES).split("#");
  QStringList notFound;

  const RouteLeg *lastLeg = route.isEmpty() ? nullptr : &route.getLastLeg();
  for(const QString& ident : alternates)
  {
    if(ident.isEmpty())
      continue;

    map::MapAirport ap;
    airportQuery->getAirportByIdent(ap, ident);

    if(ap.isValid())
    {
      FlightplanEntry entry;
      entryBuilder->entryFromAirport(ap, entry, true /* alternate */);
      fp.getEntries().append(entry);

      RouteLeg leg(&fp);
      leg.createFromDatabaseByEntry(route.size(), lastLeg);

      if(leg.getMapObjectType() == map::INVALID)
        // Not found in database
        qWarning() << "Entry for ident" << ident << "is not valid";

      route.append(leg);
      lastLeg = &route.getLastLeg();
    }
    else
      notFound.append(ident);
  }

  alternateErrors = notFound;
}

void RouteController::loadProceduresFromFlightplan(bool clearOldProcedureProperties)
{
  if(route.isEmpty())
    return;

  route.updateIndicesAndOffsets();

  QStringList errors;
  proc::MapProcedureLegs arrival, departure, star;
  NavApp::getProcedureQuery()->getLegsForFlightplanProperties(route.getFlightplan().getProperties(),
                                                              route.getDepartureAirportLeg().getAirport(),
                                                              route.getDestinationAirportLeg().getAirport(),
                                                              arrival, star, departure, errors);

  procedureErrors = errors;

  // SID/STAR with multiple runways are already assigned
  route.setSidProcedureLegs(departure);
  route.setStarProcedureLegs(star);
  route.setArrivalProcedureLegs(arrival);
  route.updateProcedureLegs(entryBuilder, clearOldProcedureProperties, false /* cleanup route */);
}

bool RouteController::loadFlightplanLnmStr(const QString& string)
{
  qDebug() << Q_FUNC_INFO;

  Flightplan fp;
  try
  {
    // Will throw an exception if something goes wrong
    flightplanIO->loadLnmStr(fp, string);

    // Convert altitude to local unit
    fp.setCruisingAltitude(atools::roundToInt(Unit::altFeetF(fp.getCruisingAltitude())));

    loadFlightplan(fp, atools::fs::pln::LNM_PLN, QString(), false /*quiet*/, false /*changed*/, false /*adjust alt*/);
  }
  catch(atools::Exception& e)
  {
    NavApp::deleteSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleException(e);
    return false;
  }
  catch(...)
  {
    NavApp::deleteSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
    return false;
  }
  return true;
}

bool RouteController::loadFlightplan(const QString& filename)
{
  qDebug() << Q_FUNC_INFO << filename;
  Flightplan fp;
  try
  {
    qDebug() << Q_FUNC_INFO << "loadFlightplan" << filename;
    // Will throw an exception if something goes wrong
    atools::fs::pln::FileFormat format = flightplanIO->load(fp, filename);
    // qDebug() << "Flight plan custom data" << newFlightplan.getProperties();

    if(fp.getEntries().size() <= 2 &&
       (format == atools::fs::pln::FMS3 || format == atools::fs::pln::FMS11))
    {
      NavApp::deleteSplashScreen();
      atools::gui::Dialog(mainWindow).showInfoMsgBox(lnm::ACTIONS_SHOW_LOAD_FMS_ALT_WARN,
                                                     tr("FMS flight plan has no intermediate waypoints.<br/><br/>"
                                                        "Can therefore not determine the cruising altitude.<br/>"
                                                        "Adjust it manually."),
                                                     tr("Do not &show this dialog again."));
      fp.setCruisingAltitude(atools::roundToInt(Unit::altFeetF(10000.f)));
    }
    else
      // Convert altitude to local unit
      fp.setCruisingAltitude(atools::roundToInt(Unit::altFeetF(fp.getCruisingAltitude())));

    loadFlightplan(fp, format, filename, false /*quiet*/, false /*changed*/, false /*adjust alt*/);
  }
  catch(atools::Exception& e)
  {
    NavApp::deleteSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleException(e);
    return false;
  }
  catch(...)
  {
    NavApp::deleteSplashScreen();
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
    return false;
  }
  return true;
}

bool RouteController::insertFlightplan(const QString& filename, int insertBefore)
{
  qDebug() << Q_FUNC_INFO << filename << insertBefore;

  Flightplan flightplan;
  pln::Flightplan& routePlan = route.getFlightplan();
  try
  {
    // Will throw an exception if something goes wrong
    flightplanIO->load(flightplan, filename);

    // Convert altitude to local unit
    flightplan.setCruisingAltitude(atools::roundToInt(Unit::altFeetF(flightplan.getCruisingAltitude())));

    RouteCommand *undoCommand = preChange(insertBefore >= route.getDestinationAirportLegIndex() ?
                                          tr("Insert") : tr("Append"));

    bool beforeDestInsert = false, beforeDepartPrepend = false, afterDestAppend = false, middleInsert = false;
    int insertPosSelection = insertBefore;

    if(insertBefore >= route.getSizeWithoutAlternates())
    {
      // Append ================================================================
      afterDestAppend = true;

      // Remove arrival legs and properties
      route.removeProcedureLegs(proc::PROCEDURE_ARRIVAL_ALL);

      // Remove alternates and properties
      route.removeAlternateLegs();
      route.clearFlightplanAlternateProperties();

      // Start of selection without arrival procedures
      insertPosSelection = route.size();

      // Append flight plan to current flightplan object - route is updated later
      for(const FlightplanEntry& entry : flightplan.getEntries())
        routePlan.getEntries().append(entry);

      // Appended after destination airport
      routePlan.setDestinationName(flightplan.getDestinationName());
      routePlan.setDestinationIdent(flightplan.getDestinationIdent());
      routePlan.setDestinationPosition(flightplan.getDestinationPosition());

      // Copy any STAR and arrival procedure properties from the loaded plan
      atools::fs::pln::copyArrivalProcedureProperties(routePlan.getProperties(),
                                                      flightplan.getProperties());
      atools::fs::pln::copyStarProcedureProperties(routePlan.getProperties(),
                                                   flightplan.getProperties());

      // Load alternates from appended plan if any
      atools::fs::pln::copyAlternateProperties(routePlan.getProperties(),
                                               flightplan.getProperties());
    }
    else
    {
      // Insert ================================================================
      if(insertBefore == 0)
      {
        // Insert before departure ==============
        beforeDepartPrepend = true;

        // Remove departure legs and properties
        route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);

        // Added before departure airport
        routePlan.setDepartureName(flightplan.getDepartureName());
        routePlan.setDepartureIdent(flightplan.getDepartureIdent());
        routePlan.setDeparturePosition(flightplan.getDeparturePosition(),
                                       flightplan.getEntries().first().getPosition().getAltitude());
        routePlan.setDepartureParkingPosition(flightplan.getDepartureParkingPosition(),
                                              flightplan.getDepartureParkingPosition().getAltitude(),
                                              flightplan.getDepartureParkingHeading());
        routePlan.setDepartureParkingName(flightplan.getDepartureParkingName());
        routePlan.setDepartureParkingType(flightplan.getDepartureParkingType());

        // Copy SID properties from source
        atools::fs::pln::copySidProcedureProperties(routePlan.getProperties(),
                                                    flightplan.getProperties());

        route.eraseAirway(1);
      }
      else if(insertBefore >= route.getSizeWithoutAlternates() - 1)
      {
        // Insert before destination ==============
        beforeDestInsert = true;

        // Remove all arrival legs and properties
        route.removeProcedureLegs(proc::PROCEDURE_ARRIVAL_ALL);

        // Correct insert position and start of selection for removed arrival legs
        insertBefore = route.getSizeWithoutAlternates() - 1;
        insertPosSelection = insertBefore;

        // No procedures taken from the loaded plan
      }
      else
      {
        // Insert in the middle ==============
        middleInsert = true;

        // No airway at start leg
        route.eraseAirway(insertBefore);

        // No procedures taken from the loaded plan
      }

      // Copy new legs to the flightplan object only - update route later
      for(auto it = flightplan.getEntries().rbegin(); it != flightplan.getEntries().rend(); ++it)
        routePlan.getEntries().insert(insertBefore, *it);
    }

    // Clear procedure legs from the flightplan object
    routePlan.removeNoSaveEntries();

    // Clear procedure structures
    route.clearProcedures(proc::PROCEDURE_ALL);

    // Clear procedure legs from route object only since indexes are not consistent now
    route.clearProcedureLegs(proc::PROCEDURE_ALL, true /* clear route */, false /* clear flight plan */);

    // Rebuild all legs from flightplan object and properties
    route.createRouteLegsFromFlightplan();

    // Load procedures and add legs
    loadProceduresFromFlightplan(true /* clear old procedure properties */);
    loadAlternateFromFlightplan();
    route.updateAll();
    route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
    route.updateLegAltitudes();

    updateActiveLeg();
    updateTableModel();

    postChange(undoCommand);

    // Select newly imported flight plan legs
    if(afterDestAppend)
      selectRange(insertPosSelection, route.size() - 1);
    else if(beforeDepartPrepend)
      selectRange(0, flightplan.getEntries().size() + route.getLastIndexOfDepartureProcedure() - 1);
    else if(beforeDestInsert)
      selectRange(insertPosSelection, route.size() - 2 - route.getNumAlternateLegs());
    else if(middleInsert)
      selectRange(insertPosSelection, insertPosSelection + flightplan.getEntries().size() - 1);

    updateMoveAndDeleteActions();

    emit routeChanged(true);
  }
  catch(atools::Exception& e)
  {
    atools::gui::ErrorHandler(mainWindow).handleException(e);
    return false;
  }
  catch(...)
  {
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
    return false;
  }
  return true;
}

void RouteController::saveFlightplanLnmExported(const QString& filename)
{
  qDebug() << Q_FUNC_INFO << filename;

  // Change name to exported filename
  routeFilename = filename;

  fileDepartureIdent = route.getFlightplan().getDepartureIdent();
  fileDestinationIdent = route.getFlightplan().getDestinationIdent();
  fileIfrVfr = route.getFlightplan().getFlightplanType();
  fileCruiseAlt = route.getCruisingAltitudeFeet();

  // Set format to original route since it is saved as LNM now
  route.getFlightplan().setLnmFormat(true);

  // Set clean undo state index since QUndoStack only returns weird values
  undoIndexClean = undoIndex;
  undoStack->setClean();

  NavApp::updateWindowTitle();
}

bool RouteController::saveFlightplanLnmAs(const QString& filename)
{
  qDebug() << Q_FUNC_INFO << filename;
  routeFilename = filename;
  return saveFlightplanLnmInternal();
}

pln::Flightplan RouteController::getFlightplanForSelection() const
{
  QList<int> rows = getSelectedRows(false /* reverse */);

  Route saveRoute(route);
  saveRoute.removeAllExceptRange(rows.first(), rows.last());
  saveRoute.updateIndicesAndOffsets();

  Flightplan saveplan = saveRoute.getFlightplan();
  saveplan.adjustDepartureAndDestination(true /* force */); // Fill departure and destination fields
  return saveplan;
}

bool RouteController::saveFlightplanLnmAsSelection(const QString& filename)
{
  // Range must not contains procedures or alternates.
  QList<int> rows = getSelectedRows(false /* reverse */);
  qDebug() << Q_FUNC_INFO << filename << rows;
  return saveFlightplanLnmSelectionAs(filename, rows.first(), rows.last());
}

bool RouteController::saveFlightplanLnm()
{
  qDebug() << Q_FUNC_INFO << routeFilename;
  return saveFlightplanLnmInternal();
}

bool RouteController::saveFlightplanLnmSelectionAs(const QString& filename, int from, int to) const
{
  try
  {
    // Create a copy
    Route saveRoute(route);
    saveRoute.updateRouteCycleMetadata();

    // Remove all legs except the ones to save
    saveRoute.removeAllExceptRange(from, to);
    saveRoute.updateIndicesAndOffsets();

    // Procedures are never saved for a selection
    saveRoute.removeProcedureLegs();

    // Get plan without alternates and all altitude values erased
    Flightplan saveplan =
      saveRoute.zeroedAltitudes().adjustedToOptions(rf::DEFAULT_OPTS_LNMPLN_SAVE_SELECTED).getFlightplan();
    saveplan.adjustDepartureAndDestination(true /* force */);

    // Remove original comment
    saveplan.setComment(QString());

    // Add performance file type and name ===============
    assignFlightplanPerfProperties(saveplan);

    // Save LNMPLN - Will throw an exception if something goes wrong
    flightplanIO->saveLnm(saveplan, filename);
  }
  catch(atools::Exception& e)
  {
    atools::gui::ErrorHandler(mainWindow).handleException(e);
    return false;
  }
  catch(...)
  {
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
    return false;
  }
  return true;
}

bool RouteController::saveFlightplanLnmInternal()
{
  try
  {
    // Update AIRAC cycle
    route.updateRouteCycleMetadata();

    // Copy loaded procedures back to properties to ensure that only valid ones are saved
    // Additionally remove duplicate waypoint
    route.updateProcedureLegs(entryBuilder, true /* clear old procedure properties */, false /* cleanup route */);

    // Create a copy which allows to change altitude
    // Copy altitudes to flight plan entries
    Flightplan flightplan = route.updatedAltitudes().adjustedToOptions(rf::DEFAULT_OPTS_LNMPLN).getFlightplan();

    // Remember type, departure and destination for filename checking (save/saveAs)
    fileIfrVfr = flightplan.getFlightplanType();
    fileCruiseAlt = route.getCruisingAltitudeFeet();
    fileDepartureIdent = flightplan.getDepartureIdent();
    fileDestinationIdent = flightplan.getDestinationIdent();

    // Set cruise altitude in feet instead of local units
    flightplan.setCruisingAltitude(
      atools::roundToInt(Unit::rev(static_cast<float>(flightplan.getCruisingAltitude()), Unit::altFeetF)));

    // Add performance file type and name ===============
    assignFlightplanPerfProperties(flightplan);

    // Save LNMPLN - Will throw an exception if something goes wrong
    flightplanIO->saveLnm(flightplan, routeFilename);

    // Set format to original route since it is saved as LNM now
    route.getFlightplan().setLnmFormat(true);

    // Set clean undo state index since QUndoStack only returns weird values
    undoIndexClean = undoIndex;
    undoStack->setClean();
    NavApp::updateWindowTitle();
    qDebug() << "saveFlightplan undoIndex" << undoIndex << "undoIndexClean" << undoIndexClean;
  }
  catch(atools::Exception& e)
  {
    atools::gui::ErrorHandler(mainWindow).handleException(e);
    return false;
  }
  catch(...)
  {
    atools::gui::ErrorHandler(mainWindow).handleUnknownException();
    return false;
  }
  return true;
}

void RouteController::calculateDirect()
{
  qDebug() << Q_FUNC_INFO;

  // Stop any background tasks
  beforeRouteCalc();

  RouteCommand *undoCommand = preChange(tr("Direct Calculation"));

  route.removeRouteLegs();

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
  route.updateLegAltitudes();

  updateTableModel();
  updateMoveAndDeleteActions();
  postChange(undoCommand);
  emit routeChanged(true);
  NavApp::setStatusMessage(tr("Calculated direct flight plan."));
}

void RouteController::beforeRouteCalc()
{
  routeAltDelayTimer.stop();
  emit preRouteCalc();
}

void RouteController::calculateRouteWindowSelection()
{
  qDebug() << Q_FUNC_INFO;
  routeWindow->showForSelectionCalculation();
  routeWindow->setCruisingAltitudeFt(route.getCruisingAltitudeFeet());
  NavApp::showRouteCalc();
}

void RouteController::calculateRouteWindowFull()
{
  qDebug() << Q_FUNC_INFO;
  routeWindow->showForFullCalculation();
  routeWindow->setCruisingAltitudeFt(route.getCruisingAltitudeFeet());
  NavApp::showRouteCalc();
}

void RouteController::calculateRoute()
{
  qDebug() << Q_FUNC_INFO;

  atools::routing::RouteNetwork *net = nullptr;
  QString command;
  atools::routing::Modes mode = atools::routing::MODE_NONE;
  bool fetchAirways = false;

  // Build configuration for route finder =======================================
  if(routeWindow->getRoutingType() == rd::AIRWAY)
  {
    net = routeNetworkAirway;
    fetchAirways = true;

    // Airway preference =======================================
    switch(routeWindow->getAirwayRoutingType())
    {
      case rd::BOTH:
        command = tr("Airway Flight Plan Calculation");
        mode = atools::routing::MODE_AIRWAY_WAYPOINT;
        break;

      case rd::VICTOR:
        command = tr("Low altitude airway Flight Plan Calculation");
        mode = atools::routing::MODE_VICTOR_WAYPOINT;
        break;

      case rd::JET:
        command = tr("High altitude airway Flight Plan Calculation");
        mode = atools::routing::MODE_JET_WAYPOINT;
        break;
    }

    // Airway/waypoint preference =======================================
    int pref = routeWindow->getAirwayWaypointPreference();
    if(pref == RouteCalcWindow::AIRWAY_WAYPOINT_PREF_MIN)
      mode &= ~atools::routing::MODE_WAYPOINT;
    else if(pref == RouteCalcWindow::AIRWAY_WAYPOINT_PREF_MAX)
      mode &= ~atools::routing::MODE_AIRWAY;

    // RNAV setting
    if(routeWindow->isAirwayNoRnav())
      mode |= atools::routing::MODE_NO_RNAV;

    // Use tracks like NAT or PACOTS
    if(routeWindow->isUseTracks())
      mode |= atools::routing::MODE_TRACK;
  }
  else if(routeWindow->getRoutingType() == rd::RADIONNAV)
  {
    // Radionav settings ========================================
    command = tr("Radionnav Flight Plan Calculation");
    fetchAirways = false;
    net = routeNetworkRadio;
    mode = atools::routing::MODE_RADIONAV_VOR;
    if(routeWindow->isRadionavNdb())
      mode |= atools::routing::MODE_RADIONAV_NDB;
  }

  if(!net->isLoaded())
  {
    atools::routing::RouteNetworkLoader loader(NavApp::getDatabaseNav(), NavApp::getDatabaseTrack());
    loader.load(net);
  }

  atools::routing::RouteFinder routeFinder(net);
  routeFinder.setCostFactorForceAirways(routeWindow->getAirwayPreferenceCostFactor());

  int fromIdx = -1, toIdx = -1;
  if(routeWindow->isCalculateSelection())
  {
    fromIdx = routeWindow->getRouteRangeFromIndex();
    toIdx = routeWindow->getRouteRangeToIndex();

    // Disable certain optimizations in route finder - use nearest underlying point as start for departure position
    mode |= atools::routing::MODE_POINT_TO_POINT;
  }

  if(route.hasAnySidProcedure())
    // Disable certain optimizations in route finder - use nearest underlying point as start for departure position
    mode |= atools::routing::MODE_POINT_TO_POINT;

  if(calculateRouteInternal(&routeFinder, command, fetchAirways, routeWindow->getCruisingAltitudeFt(),
                            fromIdx, toIdx, mode))
    NavApp::setStatusMessage(tr("Calculated flight plan."));
  else
    NavApp::setStatusMessage(tr("No route found."));

  routeWindow->updateWidgets();
}

void RouteController::clearAirwayNetworkCache()
{
  routeNetworkAirway->clear();
}

/* Calculate a flight plan to all types */
bool RouteController::calculateRouteInternal(atools::routing::RouteFinder *routeFinder, const QString& commandName,
                                             bool fetchAirways, float altitudeFt, int fromIndex, int toIndex,
                                             atools::routing::Modes mode)
{
  qDebug() << Q_FUNC_INFO;
  bool calcRange = fromIndex != -1 && toIndex != -1;
  int oldRouteSize = route.size();

  // Stop any background tasks
  beforeRouteCalc();

  Flightplan& flightplan = route.getFlightplan();

  // Load network from database if not already done
  QGuiApplication::setOverrideCursor(Qt::WaitCursor);

  Pos departurePos, destinationPos;

  if(calcRange)
  {
    fromIndex = std::max(route.getLastIndexOfDepartureProcedure(), fromIndex);
    toIndex = std::min(route.getDestinationIndexBeforeProcedure(), toIndex);

    departurePos = route.value(fromIndex).getPosition();
    destinationPos = route.value(toIndex).getPosition();
  }
  else
  {
    departurePos = route.getLastLegOfDepartureProcedure().getPosition();
    destinationPos = route.getDestinationBeforeProcedure().getPosition();
  }

  // ===================================================================
  // Set up a progress dialog which shows for all calculations taking more than half a second
  QProgressDialog progress(tr("Calculating Flight Plan ..."), tr("Cancel"), 0, 0, mainWindow);
  progress.setWindowTitle(tr("Little Navmap - Calculating Flight Plan"));
  progress.setWindowFlags(progress.windowFlags() & ~Qt::WindowContextHelpButtonHint);
  progress.setWindowModality(Qt::ApplicationModal);
  progress.setMinimumDuration(500);

  bool dialogShown = false, canceled = false;
  routeFinder->setProgressCallback([&progress, &canceled, &dialogShown](int distToDest, int currentDistToDest) -> bool
  {
    QApplication::processEvents();
    progress.setMaximum(distToDest);
    progress.setValue(distToDest - currentDistToDest);
    canceled = progress.wasCanceled();

    if(!dialogShown && progress.isVisible())
    {
      // Dialog is shown - remove wait cursor
      dialogShown = true;
      QGuiApplication::restoreOverrideCursor();
    }

    return !canceled;
  });

  // Calculate the route - calls above lambda ================================================
  bool found = routeFinder->calculateRoute(departurePos, destinationPos, atools::roundToInt(altitudeFt), mode);

  if(!dialogShown)
    QGuiApplication::restoreOverrideCursor();

  qDebug() << Q_FUNC_INFO << "found" << found << "canceled" << canceled;

  // Hide dialog
  progress.reset();

  // Create wait cursor if calculation takes too long
  QGuiApplication::setOverrideCursor(Qt::WaitCursor);

  float distance = 0.f;
  QVector<RouteEntry> calculatedRoute;

  if(found && !canceled)
  {
    // A route was found

    // Fetch waypoints
    RouteExtractor extractor(routeFinder);
    extractor.extractRoute(calculatedRoute, distance);
    qDebug() << Q_FUNC_INFO << "Extracted size" << calculatedRoute.size();

    found = calculatedRoute.size() > 0;
  }

  if(found && !canceled)
  {
    // Compare to direct connection and check if route is too long
    float directDistance = departurePos.distanceMeterTo(destinationPos);
    float ratio = distance / directDistance;
    qDebug() << "route distance" << QString::number(distance, 'f', 0)
             << "direct distance" << QString::number(directDistance, 'f', 0) << "ratio" << ratio;

    if(ratio < MAX_DISTANCE_DIRECT_RATIO)
    {
      // Start undo
      RouteCommand *undoCommand = preChange(commandName);
      int numAlternateLegs = route.getNumAlternateLegs();

      QList<FlightplanEntry>& entries = flightplan.getEntries();

      if(calcRange)
      {
        entries[toIndex].setAirway(QString());
        entries[toIndex].setFlag(atools::fs::pln::entry::TRACK, false);
        entries.erase(flightplan.getEntries().begin() + fromIndex + 1, flightplan.getEntries().begin() + toIndex);
      }
      else
        // Erase all but start and destination
        entries.erase(flightplan.getEntries().begin() + 1, entries.end() - numAlternateLegs - 1);

      int idx = 1;
      // Create flight plan entries - will be copied later to the route map objects
      for(const RouteEntry& routeEntry : calculatedRoute)
      {
        FlightplanEntry flightplanEntry;
        entryBuilder->buildFlightplanEntry(routeEntry.ref.id, atools::geo::EMPTY_POS, routeEntry.ref.objType,
                                           flightplanEntry, fetchAirways);
        if(fetchAirways && routeEntry.airwayId != -1)
          // Get airway by id - needed to fetch the name first
          updateFlightplanEntryAirway(routeEntry.airwayId, flightplanEntry);

        if(calcRange)
          entries.insert(flightplan.getEntries().begin() + fromIndex + idx, flightplanEntry);
        else
          entries.insert(entries.end() - numAlternateLegs - 1, flightplanEntry);
        idx++;
      }

      // Remove procedure points from flight plan
      flightplan.removeNoSaveEntries();

      // Copy flight plan to route object
      route.createRouteLegsFromFlightplan();

      // Reload procedures from properties
      loadProceduresFromFlightplan(true /* clear old procedure properties */);
      loadAlternateFromFlightplan();
      QGuiApplication::restoreOverrideCursor();

      // Remove duplicates in flight plan and route
      route.updateAll();

      // Set altitude in local units
      flightplan.setCruisingAltitude(atools::roundToInt(Unit::altFeetF(altitudeFt)));

      route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);

      updateActiveLeg();

      route.updateLegAltitudes();

      updateTableModel();
      updateMoveAndDeleteActions();

      postChange(undoCommand);
      NavApp::updateWindowTitle();

#ifdef DEBUG_INFORMATION
      qDebug() << flightplan;
#endif

      NavApp::updateErrorLabels();

      if(calcRange)
      {
        // will also update route window
        int newToIndex = toIndex - (oldRouteSize - route.size());
        selectRange(fromIndex, newToIndex);
      }

      emit routeChanged(true);
    }
    else
      // Too long
      found = false;
  }

  QGuiApplication::restoreOverrideCursor();
  if(!found && !canceled)
    atools::gui::Dialog(mainWindow).showInfoMsgBox(lnm::ACTIONS_SHOWROUTE_ERROR,
                                                   tr("Cannot calculate flight plan.\n\n"
                                                      "Try another calculation type,\n"
                                                      "change the cruise altitude or\n"
                                                      "create the flight plan manually."),
                                                   tr("Do not &show this dialog again."));
#ifdef DEBUG_INFORMATION
  qDebug() << Q_FUNC_INFO << route;
#endif

  return found;
}

void RouteController::adjustFlightplanAltitude()
{
  qDebug() << Q_FUNC_INFO;

  if(route.isEmpty())
    return;

  Flightplan& fp = route.getFlightplan();
  int alt = route.getAdjustedAltitude(fp.getCruisingAltitude());

  if(alt != fp.getCruisingAltitude())
  {
    RouteCommand *undoCommand = nullptr;

    // if(route.getFlightplan().canSaveAltitude())
    undoCommand = preChange(tr("Adjust altitude"), rctype::ALTITUDE);
    fp.setCruisingAltitude(alt);

    updateTableModel();

    // Need to update again after updateAll and altitude change
    route.updateLegAltitudes();

    postChange(undoCommand);

    NavApp::updateWindowTitle();
    NavApp::updateErrorLabels();

    if(!route.isEmpty())
      emit routeAltitudeChanged(route.getCruisingAltitudeFeet());

    NavApp::setStatusMessage(tr("Adjusted flight plan altitude."));
  }
}

void RouteController::reverseRoute()
{
  qDebug() << Q_FUNC_INFO;

  RouteCommand *undoCommand = preChange(tr("Reverse"), rctype::REVERSE);

  // Remove all procedures and properties
  route.removeProcedureLegs(proc::PROCEDURE_ALL);
  route.removeAlternateLegs();

  atools::fs::pln::Flightplan& flightplan = route.getFlightplan();
  auto& entries = route.getFlightplan().getEntries();
  std::reverse(entries.begin(), entries.end());

  QString depName = flightplan.getDepartureName();
  QString depIdent = flightplan.getDepartureIdent();
  flightplan.setDepartureName(flightplan.getDestinationName());
  flightplan.setDepartureIdent(flightplan.getDestinationIdent());

  flightplan.setDestinationName(depName);
  flightplan.setDestinationIdent(depIdent);

  // Overwrite parking position with airport position
  flightplan.setDeparturePosition(entries.first().getPosition());
  flightplan.setDepartureParkingPosition(entries.first().getPosition(),
                                         atools::fs::pln::INVALID_ALTITUDE, atools::fs::pln::INVALID_HEADING);
  flightplan.setDepartureParkingName(QString());
  flightplan.setDepartureParkingType(atools::fs::pln::NO_POS);

  // Erase all airways to avoid wrong direction travel against one way
  for(int i = 0; i < entries.size(); i++)
    entries[i].setAirway(QString());
  flightplan.getProperties().remove(atools::fs::pln::PROCAIRWAY);

  route.createRouteLegsFromFlightplan();
  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
  route.updateLegAltitudes();
  updateStartPositionBestRunway(true /* force */, false /* undo */);

  updateActiveLeg();
  updateTableModel();
  updateMoveAndDeleteActions();

  postChange(undoCommand);
  emit routeChanged(true);
  NavApp::setStatusMessage(tr("Reversed flight plan."));
}

void RouteController::preDatabaseLoad()
{
  loadingDatabaseState = true;
  routeAltDelayTimer.stop();

  // Reset active to avoid crash when indexes change
  route.resetActive();
  highlightNextWaypoint(route.getActiveLegIndex());

  routeWindow->preDatabaseLoad();
}

void RouteController::postDatabaseLoad()
{
  routeNetworkRadio->clear();
  routeNetworkAirway->clear();

  // Remove the legs but keep the properties
  route.clearProcedures(proc::PROCEDURE_ALL);
  route.clearProcedureLegs(proc::PROCEDURE_ALL);

  route.createRouteLegsFromFlightplan();
  loadProceduresFromFlightplan(false /* clear old procedure properties */);
  // loadAlternateFromFlightplan();// not needed - alternates are already stored in dummy legs
  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
  route.updateLegAltitudes();

  // Update runway or parking if one of these has changed due to the database switch
  Flightplan& flightplan = route.getFlightplan();
  if(!flightplan.getEntries().isEmpty() &&
     flightplan.getEntries().first().getWaypointType() == atools::fs::pln::entry::AIRPORT &&
     flightplan.getDepartureParkingName().isEmpty())
    updateStartPositionBestRunway(false /* force */, false /* undo */);

  updateActiveLeg();

  // Need to update model to reflect the flight plan changes before other methods call updateModelHighlights
  updateTableModel();
  updateMoveAndDeleteActions();

  NavApp::updateErrorLabels();
  routeAltChangedDelayed();
  route.updateRouteCycleMetadata();

  routeWindow->postDatabaseLoad();

  NavApp::updateWindowTitle();
  loadingDatabaseState = false;
}

/* Double click into table view */
void RouteController::doubleClick(const QModelIndex& index)
{
  qDebug() << Q_FUNC_INFO;
  if(index.isValid())
  {
    qDebug() << "mouseDoubleClickEvent";
    showAtIndex(index.row(), true /* info */, true /* map */, true /* doubleClick */);
  }
}

void RouteController::showAtIndex(int index, bool info, bool map, bool doubleClick)
{
  if(index >= 0 && index < map::INVALID_INDEX_VALUE)
  {
    const RouteLeg& routeLeg = route.value(index);
    if(routeLeg.isValid())
    {
      if(map)
      {
        if(routeLeg.getMapObjectType() == map::AIRPORT)
          emit showRect(routeLeg.getAirport().bounding, doubleClick);
        else
          emit showPos(routeLeg.getPosition(), 0.f, doubleClick);
      }

      if(info)
        showInformationInternal(routeLeg);
    }
  }
}

void RouteController::updateMoveAndDeleteActions()
{
  // Disable all per default
  Ui::MainWindow *ui = NavApp::getMainUi();
  ui->actionRouteLegUp->setEnabled(false);
  ui->actionRouteLegDown->setEnabled(false);
  ui->actionRouteDeleteLeg->setEnabled(false);

  // Empty - nothing to move or delete
  if(model->rowCount() == 0)
    return;

  // One or more legs - check for details like procedures or procedure boundary
  QItemSelectionModel *sm = view->selectionModel();
  if(sm != nullptr && sm->hasSelection())
  {
    bool containsProc = false, containsAlternate = false, moveDownTouchesProc = false, moveUpTouchesProc = false,
         moveDownTouchesAlt = false, moveUpTouchesAlt = false,
         moveDownLeavesAlt = false, moveUpLeavesAlt = false;
    // Ordered from top to bottom
    QList<int> rows = getSelectedRows(false);

    for(int row : rows)
    {
      containsProc |= route.value(row).isAnyProcedure();
      containsAlternate |= route.value(row).isAlternate();
    }

    moveUpTouchesProc = rows.first() > 0 && route.value(rows.first() - 1).isAnyProcedure();
    moveDownTouchesProc = rows.last() < route.size() - 1 && route.value(rows.last() + 1).isAnyProcedure();

    moveUpTouchesAlt = rows.first() > 0 && route.value(rows.first() - 1).isAlternate();
    moveDownTouchesAlt = rows.last() < route.size() - 1 && route.value(rows.last() + 1).isAlternate();

    moveUpLeavesAlt = rows.first() > 0 && !route.value(rows.first() - 1).isAlternate();
    moveDownLeavesAlt = rows.last() >= route.size() - 1 || !route.value(rows.last() + 1).isAlternate();

    if(rows.size() == 1 && containsAlternate)
    {
      ui->actionRouteLegUp->setEnabled(!moveUpLeavesAlt);
      ui->actionRouteLegDown->setEnabled(!moveDownLeavesAlt);
      ui->actionRouteDeleteLeg->setEnabled(true);
    }
    else if(model->rowCount() > 1)
    {
      ui->actionRouteDeleteLeg->setEnabled(true);
      ui->actionRouteLegUp->setEnabled(sm->hasSelection() && !sm->isRowSelected(0, QModelIndex()) &&
                                       !containsProc && !containsAlternate && !moveUpTouchesProc && !moveUpTouchesAlt);

      ui->actionRouteLegDown->setEnabled(sm->hasSelection() &&
                                         !sm->isRowSelected(model->rowCount() - 1, QModelIndex()) &&
                                         !containsProc && !containsAlternate && !moveDownTouchesProc &&
                                         !moveDownTouchesAlt);
    }
    else if(model->rowCount() == 1)
      // Only one waypoint - nothing to move
      ui->actionRouteDeleteLeg->setEnabled(true);
  }
}

/* From context menu */
void RouteController::showInformationMenu()
{
  if(!hasTableSelection())
    return;

  qDebug() << Q_FUNC_INFO;
  QModelIndex index = view->currentIndex();
  if(index.isValid())
    showInformationInternal(route.value(index.row()));
}

void RouteController::showInformationInternal(const RouteLeg& routeLeg)
{

  if(routeLeg.isAnyProcedure())
  {
    if(routeLeg.getProcedureLeg().navaids.hasTypes(map::AIRPORT | map::WAYPOINT | map::VOR | map::NDB))
      emit showInformation(routeLeg.getProcedureLeg().navaids);
  }
  else
  {
    map::MapResult result;
    mapQuery->getMapObjectById(result, routeLeg.getMapObjectType(), map::AIRSPACE_SRC_NONE, routeLeg.getId(),
                               false /* airport from nav database */);
    emit showInformation(result);
  }
}

/* From context menu */
void RouteController::showProceduresMenu()
{
  if(!hasTableSelection())
    return;

  QModelIndex index = view->currentIndex();
  if(index.isValid())
  {
    const RouteLeg& routeLeg = route.value(index.row());

    if(routeLeg.isValidWaypoint() && routeLeg.getMapObjectType() == map::AIRPORT)
    {
      bool departureFilter, arrivalFilter;
      route.getAirportProcedureFlags(routeLeg.getAirport(), index.row(), departureFilter, arrivalFilter);
      emit showProcedures(routeLeg.getAirport(), departureFilter, arrivalFilter);
    }
  }
}

/* From context menu */
void RouteController::showProceduresMenuCustom()
{
  if(!hasTableSelection())
    return;

  QModelIndex index = view->currentIndex();
  if(index.isValid())
  {
    const RouteLeg& routeLeg = route.value(index.row());
    if(routeLeg.isValidWaypoint() && routeLeg.getMapObjectType() == map::AIRPORT)
      showProceduresCustom(routeLeg.getAirport());
  }
}

/* From context menu */
void RouteController::showOnMapMenu()
{
  if(!hasTableSelection())
    return;

  QModelIndex index = view->currentIndex();
  if(index.isValid())
  {
    const RouteLeg& routeLeg = route.value(index.row());

    if(routeLeg.getMapObjectType() == map::AIRPORT)
      emit showRect(routeLeg.getAirport().bounding, false);
    else
      emit showPos(routeLeg.getPosition(), 0.f, false);

    if(routeLeg.getMapObjectType() == map::AIRPORT)
      NavApp::setStatusMessage(tr("Showing airport on map."));
    else
      NavApp::setStatusMessage(tr("Showing navaid on map."));
  }
}

void RouteController::visibleColumnsTriggered()
{
  qDebug() << Q_FUNC_INFO;

  ChoiceDialog dialog(mainWindow, QApplication::applicationName() + tr(" - Flight Plan Table"), QString(),
                      tr("Select columns to show in flight plan table"),
                      lnm::ROUTE_FLIGHTPLAN_COLUMS_DIALOG, "FLIGHTPLAN.html#flight-plan-table-columns");

  QHeaderView *header = view->horizontalHeader();
  for(int col = rcol::FIRST_COLUMN; col <= rcol::LAST_COLUMN; col++)
    dialog.addCheckBox(col, Unit::replacePlaceholders(routeColumns.at(col)).replace("\n", " "),
                       routeColumnTooltips.at(col),
                       !header->isSectionHidden(col));

  if(dialog.exec() == QDialog::Accepted)
  {
    for(int col = rcol::LAST_COLUMN; col >= rcol::FIRST_COLUMN; col--)
      header->setSectionHidden(col, !dialog.isChecked(col));

    updateModelTimeFuelWind();
    updateModelHighlights();
    highlightNextWaypoint(route.getActiveLegIndexCorrected());
  }
}

void RouteController::activateLegTriggered()
{
  if(hasTableSelection())
    activateLegManually(selectedRows.first());
}

void RouteController::helpClicked()
{
  atools::gui::HelpHandler::openHelpUrlWeb(mainWindow, lnm::helpOnlineUrl + "FLIGHTPLAN.html",
                                           lnm::helpLanguageOnline());
}

void RouteController::selectAllTriggered()
{
  view->selectAll();
}

bool RouteController::canCalcSelection()
{
  // Check if selected rows contain a procedure or if a procedure is between first and last selection
  if(selectedRows.size() > 1)
    return route.canCalcSelection(selectedRows.first(), selectedRows.last());

  return false;
}

bool RouteController::canSaveSelection()
{
  // Check if selected rows contain a procedure or if a procedure is between first and last selection
  if(selectedRows.size() > 1)
    return route.canSaveSelection(selectedRows.first(), selectedRows.last());

  return false;
}

void RouteController::tableContextMenu(const QPoint& pos)
{
  Ui::MainWindow *ui = NavApp::getMainUi();
  contextMenuOpen = true;
  QPoint menuPos = QCursor::pos();
  // Use widget center if position is not inside widget
  if(!ui->tableViewRoute->rect().contains(ui->tableViewRoute->mapFromGlobal(QCursor::pos())))
    menuPos = ui->tableViewRoute->mapToGlobal(ui->tableViewRoute->rect().center());

  // Move menu position off the cursor to avoid accidental selection on touchpads
  menuPos += QPoint(3, 3);

  qDebug() << "tableContextMenu";

  // Save text which will be changed below
  atools::gui::ActionTextSaver saver({ui->actionMapRangeRings, ui->actionMapNavaidRange,
                                      ui->actionRouteEditUserWaypoint, ui->actionRouteShowApproaches,
                                      ui->actionRouteShowApproachesCustom, ui->actionRouteDeleteLeg,
                                      ui->actionRouteInsert, ui->actionMapTrafficPattern, ui->actionMapHold});

  // Re-enable actions on exit to allow keystrokes
  atools::gui::ActionStateSaver stateSaver(
  {
    ui->actionRouteShowInformation, ui->actionRouteShowApproaches, ui->actionRouteShowApproachesCustom,
    ui->actionRouteShowOnMap, ui->actionRouteActivateLeg, ui->actionRouteLegUp, ui->actionRouteLegDown,
    ui->actionRouteDeleteLeg, ui->actionRouteEditUserWaypoint, ui->actionRouteCalcSelected,
    ui->actionMapRangeRings, ui->actionMapTrafficPattern, ui->actionMapHold, ui->actionMapNavaidRange,
    ui->actionRouteTableCopy, ui->actionRouteTableSelectNothing, ui->actionRouteTableSelectAll,
    ui->actionRouteResetView, ui->actionRouteSetMark,
    ui->actionRouteInsert, ui->actionRouteTableAppend, ui->actionRouteSaveSelection});

  QModelIndex index = view->indexAt(pos);
  const RouteLeg *routeLeg = nullptr, *prevRouteLeg = nullptr;
  int row = -1;
  if(index.isValid())
  {
    row = index.row();
    routeLeg = &route.value(row);
    if(index.row() > 0)
      prevRouteLeg = &route.value(row - 1);
  }

  QMenu menu;
  menu.setToolTipsVisible(NavApp::isMenuToolTipsVisible());

  updateMoveAndDeleteActions();

  ui->actionRouteTableCopy->setEnabled(index.isValid());

  bool insert = false;

  ui->actionRouteShowApproachesCustom->setEnabled(false);
  ui->actionRouteShowApproaches->setEnabled(false);
  ui->actionRouteEditUserWaypoint->setEnabled(false);
  ui->actionRouteShowInformation->setEnabled(false);

  // Menu above a row
  if(routeLeg != nullptr)
  {
    if(routeLeg->isAnyProcedure())
    {
      if(routeLeg->getProcedureLeg().navaids.hasTypes(map::AIRPORT | map::WAYPOINT | map::VOR | map::NDB))
        ui->actionRouteShowInformation->setEnabled(true);
    }
    else
      ui->actionRouteShowInformation->setEnabled(routeLeg->isValidWaypoint() &&
                                                 routeLeg->getMapObjectType() != map::USERPOINTROUTE &&
                                                 routeLeg->getMapObjectType() != map::INVALID);

    if(routeLeg->isValidWaypoint())
    {
      if(prevRouteLeg == nullptr)
        // allow to insert before first one
        insert = true;
      else if(prevRouteLeg->isRoute() && routeLeg->isAnyProcedure() &&
              routeLeg->getProcedureType() & proc::PROCEDURE_ARRIVAL_ALL)
        // Insert between enroute waypoint and approach or STAR
        insert = true;
      else if(routeLeg->isRoute() && prevRouteLeg->isAnyProcedure() &&
              prevRouteLeg->getProcedureType() & proc::PROCEDURE_DEPARTURE)
        // Insert between SID and waypoint
        insert = true;
      else
        insert = routeLeg->isRoute();
    }

    if(routeLeg->isValidWaypoint() && routeLeg->getMapObjectType() == map::AIRPORT)
    {
      bool departureFilter, arrivalFilter, hasDeparture, hasAnyArrival, airportDeparture, airportDestination,
           airportRoundTrip;

      route.getAirportProcedureFlags(routeLeg->getAirport(), row, departureFilter, arrivalFilter, hasDeparture,
                                     hasAnyArrival, airportDeparture, airportDestination, airportRoundTrip);

      if(hasAnyArrival || hasDeparture)
      {
        if(airportDeparture && !airportRoundTrip)
        {
          if(hasDeparture)
          {
            ui->actionRouteShowApproaches->setEnabled(true);
            ui->actionRouteShowApproaches->setText(ui->actionRouteShowApproaches->text().arg(tr("Departure ")));
          }
          else
            ui->actionRouteShowApproaches->setText(tr("Show procedures (airport has no departure procedure)"));
        }
        else if(airportDestination && !airportRoundTrip)
        {
          if(hasAnyArrival)
          {
            ui->actionRouteShowApproaches->setEnabled(true);
            ui->actionRouteShowApproaches->setText(ui->actionRouteShowApproaches->text().arg(tr("Arrival ")));
          }
          else
            ui->actionRouteShowApproaches->setText(tr("Show procedures (airport has no arrival procedure)"));
        }
        else
        {
          ui->actionRouteShowApproaches->setEnabled(true);
          ui->actionRouteShowApproaches->setText(ui->actionRouteShowApproaches->text().arg(tr("all ")));
        }
      }
      else
        ui->actionRouteShowApproaches->setText(tr("Show Procedures (airport has no procedure)"));

      ui->actionRouteShowApproachesCustom->setEnabled(true);
      if(airportDestination)
        ui->actionRouteShowApproachesCustom->setText(tr("Create &Approach to Airport and insert into Flight Plan"));
      else
        ui->actionRouteShowApproachesCustom->setText(tr("Create &Approach and use Airport as Destination"));
    }
    else
    {
      ui->actionRouteShowApproaches->setText(tr("Show &Procedures"));
      ui->actionRouteShowApproachesCustom->setText(tr("Create &Approach to Airport"));
    }

    ui->actionRouteShowOnMap->setEnabled(true);
    ui->actionMapRangeRings->setEnabled(true);
    ui->actionRouteSetMark->setEnabled(true);

    // ui->actionRouteDeleteLeg->setText(route->isAnyProcedure() ?
    // tr("Delete Procedure") : tr("Delete selected Legs"));

#ifdef DEBUG_MOVING_AIRPLANE
    ui->actionRouteActivateLeg->setEnabled(routeLeg->isValidWaypoint());
#else
    ui->actionRouteActivateLeg->setEnabled(routeLeg->isValid() && NavApp::isConnected());
#endif
  }
  else
  {
    ui->actionRouteShowInformation->setEnabled(false);
    ui->actionRouteShowApproaches->setEnabled(false);
    ui->actionRouteShowApproaches->setText(tr("Show procedures"));
    ui->actionRouteActivateLeg->setEnabled(false);
    ui->actionRouteShowOnMap->setEnabled(false);
    ui->actionMapRangeRings->setEnabled(false);
    ui->actionRouteSetMark->setEnabled(false);
  }

  ui->actionRouteSaveSelection->setEnabled(canSaveSelection());

  ui->actionRouteTableAppend->setEnabled(!route.isEmpty());

  if(insert)
  {
    ui->actionRouteInsert->setEnabled(true);
    ui->actionRouteInsert->setText(ui->actionRouteInsert->text().arg(routeLeg->getDisplayIdent()));
  }
  else
  {
    ui->actionRouteInsert->setEnabled(false);
    ui->actionRouteInsert->setText(tr("Insert Flight Plan before ..."));
  }

  if(routeLeg != nullptr && routeLeg->getAirport().isValid() && !routeLeg->getAirport().noRunways())
    ui->actionMapTrafficPattern->setEnabled(true);
  else
    ui->actionMapTrafficPattern->setEnabled(false);
  ui->actionMapTrafficPattern->setText(tr("Add Airport &Traffic Pattern ..."));

  ui->actionMapHold->setEnabled(routeLeg != nullptr);
  ui->actionMapHold->setText(tr("Add &Holding ..."));

  ui->actionRouteCalcSelected->setEnabled(canCalcSelection());

  ui->actionMapNavaidRange->setEnabled(false);

  ui->actionRouteTableSelectNothing->setEnabled(
    view->selectionModel() == nullptr ? false : view->selectionModel()->hasSelection());
  ui->actionRouteTableSelectAll->setEnabled(!route.isEmpty());

  ui->actionMapNavaidRange->setText(tr("Add &Navaid Range Ring"));

  // Edit position ======================================0

  ui->actionRouteEditUserWaypoint->setText(tr("Edit Flight Plan &Position or Remarks ..."));
  if(routeLeg != nullptr)
  {
    if(routeLeg->getMapObjectType() == map::USERPOINTROUTE)
    {
      ui->actionRouteEditUserWaypoint->setEnabled(true);
      ui->actionRouteEditUserWaypoint->setText(tr("Edit Flight Plan &Position ..."));
      ui->actionRouteEditUserWaypoint->setToolTip(tr("Edit name and coordinates of user defined flight plan position"));
      ui->actionRouteEditUserWaypoint->setStatusTip(ui->actionRouteEditUserWaypoint->toolTip());
    }
    else if(route.canEditComment(row))
    {
      ui->actionRouteEditUserWaypoint->setEnabled(true);
      ui->actionRouteEditUserWaypoint->setText(tr("Edit Flight Plan &Position Remarks ..."));
      ui->actionRouteEditUserWaypoint->setToolTip(tr("Edit remarks for selected flight plan leg"));
      ui->actionRouteEditUserWaypoint->setStatusTip(ui->actionRouteEditUserWaypoint->toolTip());
    }
  }

  QList<int> selectedRouteLegIndexes;
  getSelectedRouteLegs(selectedRouteLegIndexes);
  // If there are any radio navaids in the selected list enable range menu item
  for(int idx : selectedRouteLegIndexes)
  {
    const RouteLeg& leg = route.value(idx);
    if((leg.getVor().isValid() && leg.getVor().range > 0) || (leg.getNdb().isValid() && leg.getNdb().range > 0))
    {
      ui->actionMapNavaidRange->setEnabled(true);
      break;
    }
  }

  // Update texts to give user a hint for hidden user features in the disabled menu items =====================
  QString notShown(tr(" (hidden on map)"));
  if(!NavApp::getMapMarkHandler()->isShown(map::MARK_RANGE_RINGS))
  {
    ui->actionMapRangeRings->setDisabled(true);
    ui->actionMapNavaidRange->setDisabled(true);
    ui->actionMapRangeRings->setText(ui->actionMapRangeRings->text() + notShown);
    ui->actionMapNavaidRange->setText(ui->actionMapNavaidRange->text() + notShown);
  }
  if(!NavApp::getMapMarkHandler()->isShown(map::MARK_HOLDS))
  {
    ui->actionMapHold->setDisabled(true);
    ui->actionMapHold->setText(ui->actionMapHold->text() + notShown);
  }
  if(!NavApp::getMapMarkHandler()->isShown(map::MARK_PATTERNS))
  {
    ui->actionMapTrafficPattern->setDisabled(true);
    ui->actionMapTrafficPattern->setText(ui->actionMapTrafficPattern->text() + notShown);
  }

  // ====================================================================
  menu.addAction(ui->actionRouteShowInformation);
  menu.addAction(ui->actionRouteShowApproaches);
  menu.addAction(ui->actionRouteShowApproachesCustom);
  menu.addAction(ui->actionRouteShowOnMap);
  menu.addAction(ui->actionRouteActivateLeg);
  menu.addSeparator();

  menu.addAction(ui->actionRouteFollowSelection);
  menu.addSeparator();

  menu.addAction(ui->actionRouteLegUp);
  menu.addAction(ui->actionRouteLegDown);
  menu.addAction(ui->actionRouteDeleteLeg);
  menu.addAction(ui->actionRouteEditUserWaypoint);
  menu.addSeparator();

  menu.addAction(ui->actionRouteInsert);
  menu.addAction(ui->actionRouteTableAppend);
  menu.addAction(ui->actionRouteSaveSelection);
  menu.addSeparator();

  menu.addAction(ui->actionRouteCalcSelected);
  menu.addSeparator();

  menu.addAction(ui->actionMapRangeRings);
  menu.addAction(ui->actionMapNavaidRange);
  menu.addSeparator();
  menu.addAction(ui->actionMapTrafficPattern);
  menu.addAction(ui->actionMapHold);
  menu.addSeparator();

  menu.addAction(ui->actionRouteTableCopy);
  menu.addAction(ui->actionRouteTableSelectAll);
  menu.addAction(ui->actionRouteTableSelectNothing);
  menu.addSeparator();

  menu.addAction(ui->actionRouteResetView);
  menu.addAction(ui->actionRouteVisibleColumns);
  menu.addSeparator();

  menu.addAction(ui->actionRouteSetMark);

  QAction *action = menu.exec(menuPos);
  if(action != nullptr)
    qDebug() << Q_FUNC_INFO << "selected" << action->text();
  else
    qDebug() << Q_FUNC_INFO << "no action selected";

  if(action != nullptr)
  {
    if(action == ui->actionRouteResetView)
    {
      for(int col = rcol::FIRST_COLUMN; col <= rcol::LAST_COLUMN; col++)
        view->showColumn(col);

      // Reorder columns to match model order
      QHeaderView *header = view->horizontalHeader();
      for(int i = 0; i < header->count(); i++)
        header->moveSection(header->visualIndex(i), i);

      view->resizeColumnsToContents();
      NavApp::setStatusMessage(tr("Table view reset to defaults."));
    }
    else if(action == ui->actionRouteSetMark && routeLeg != nullptr)
      emit changeMark(routeLeg->getPosition());
    else if(action == ui->actionMapRangeRings && routeLeg != nullptr)
      NavApp::getMapWidget()->addRangeRing(routeLeg->getPosition());
    else if(action == ui->actionMapTrafficPattern && routeLeg != nullptr)
      NavApp::getMapWidget()->addTrafficPattern(routeLeg->getAirport());
    else if(action == ui->actionMapHold && routeLeg != nullptr)
    {
      map::MapResult result;
      mapQuery->getMapObjectById(result, routeLeg->getMapObjectType(), map::AIRSPACE_SRC_NONE, routeLeg->getId(),
                                 false /* airport from nav*/);

      if(!result.isEmpty(map::AIRPORT | map::VOR | map::NDB | map::WAYPOINT))
        NavApp::getMapWidget()->addHold(result, atools::geo::EMPTY_POS);
      else
        NavApp::getMapWidget()->addHold(result, routeLeg->getPosition());
    }
    else if(action == ui->actionMapNavaidRange)
    {
      // Show range rings for all radio navaids
      for(int idx : selectedRouteLegIndexes)
      {
        const RouteLeg& routeLegSel = route.value(idx);
        if(routeLegSel.getNdb().isValid() || routeLegSel.getVor().isValid())
        {
          map::MapTypes type = routeLegSel.getMapObjectType();
          if(routeLegSel.isAnyProcedure())
          {
            if(routeLegSel.getNdb().isValid())
              type = map::NDB;
            if(routeLegSel.getVor().isValid())
              type = map::VOR;
          }

          if(routeLegSel.getRange() > 0)
            NavApp::getMapWidget()->addNavRangeRing(routeLegSel.getPosition(), type,
                                                    routeLegSel.getDisplayIdent(), routeLegSel.getFrequencyOrChannel(),
                                                    routeLegSel.getRange());
        }
      }
    }
    // else if(action == ui->actionMapHideRangeRings)
    // NavApp::getMapWidget()->clearRangeRingsAndDistanceMarkers(); // Connected directly
    // else if(action == ui->actionRouteEditUserWaypoint)
    // editUserWaypointName(index.row());
    // else if(action == ui->actionRouteTableAppend) // Done by signal from action
    // emit routeAppend();
    else if(action == ui->actionRouteInsert)
      emit routeInsert(row);
    else if(action == ui->actionRouteActivateLeg)
      activateLegManually(index.row());
    else if(action == ui->actionRouteCalcSelected)
      calculateRouteWindowSelection();
    // Other actions emit signals directly
  }
  contextMenuOpen = false;

  // Restart the timers for clearing the selection or moving the active to top
  updateCleanupTimer();
}

/* Activate leg manually from menu */
void RouteController::activateLegManually(int index)
{
  qDebug() << Q_FUNC_INFO << index;
  route.setActiveLeg(index);
  highlightNextWaypoint(route.getActiveLegIndex());
  // Use geometry changed flag to force redraw
  emit routeChanged(true);
}

void RouteController::resetActiveLeg()
{
  qDebug() << Q_FUNC_INFO;
  route.resetActive();
  highlightNextWaypoint(route.getActiveLegIndex());
  // Use geometry changed flag to force redraw
  emit routeChanged(true);
}

void RouteController::updateActiveLeg()
{
  route.updateActiveLegAndPos(true /* force update */, aircraft.isFlying());
}

void RouteController::editUserWaypointTriggered()
{
  if(hasTableSelection())
    editUserWaypointName(view->currentIndex().row());
}

void RouteController::editUserWaypointName(int index)
{
  qDebug() << Q_FUNC_INFO << "index" << index;

  if(index >= 0 && route.canEditComment(index))
  {
    UserWaypointDialog dialog(mainWindow, route.value(index).getFlightplanEntry());
    if(dialog.exec() == QDialog::Accepted)
    {
      RouteCommand *undoCommand = nullptr;

      // if(route.getFlightplan().canSaveUserWaypointName())
      undoCommand = preChange(tr("Waypoint Change"));

      route.getFlightplan().getEntries()[index] = dialog.getEntry();

      route.updateAll();
      route.updateLegAltitudes();

      updateActiveLeg();
      updateTableModel();
      updateMoveAndDeleteActions();

      postChange(undoCommand);
      emit routeChanged(true);
      NavApp::setStatusMessage(tr("Changed waypoint in flight plan."));
    }
  }
}

void RouteController::shownMapFeaturesChanged(map::MapTypes types)
{
  // qDebug() << Q_FUNC_INFO;
  route.setShownMapFeatures(types);
  route.setShownMapFeatures(types);
}

/* Hide or show map hightlights if dock visibility changes */
void RouteController::dockVisibilityChanged(bool visible)
{
  Q_UNUSED(visible)

  // Visible - send update to show map highlights
  // Not visible - send update to hide highlights
  tableSelectionChanged(QItemSelection(), QItemSelection());
}

bool RouteController::canCleanupTable()
{
  return !contextMenuOpen && !view->horizontalScrollBar()->isSliderDown() && !view->verticalScrollBar()->isSliderDown();
}

void RouteController::cleanupTableTimeout()
{
#ifdef DEBUG_INFORMATION
  qDebug() << Q_FUNC_INFO;
#endif

  if(NavApp::isConnectedAndAircraftFlying())
  {
    if(!canCleanupTable())
      // Cannot clean up now - restart timers
      updateCleanupTimer();
    else
    {
      opts2::Flags2 flags2 = OptionData::instance().getFlags2();
      if(hasTableSelection() && flags2.testFlag(opts2::ROUTE_CLEAR_SELECTION))
      {
        // Clear selection and move cursor to current row and first column
        QModelIndex idx = view->model()->index(view->currentIndex().row(), view->horizontalHeader()->logicalIndex(0));
        view->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::Clear);
      }

      if(flags2.testFlag(opts2::ROUTE_CENTER_ACTIVE_LEG))
        // Move active to second row
        scrollToActive();
    }
  }
}

void RouteController::clearTableSelection()
{
  view->clearSelection();
}

bool RouteController::hasTableSelection()
{
  return view->selectionModel() == nullptr ? false : view->selectionModel()->hasSelection();
}

void RouteController::updateCleanupTimer()
{
  if(NavApp::isConnectedAndAircraftFlying())
  {
    opts2::Flags2 flags2 = OptionData::instance().getFlags2();
    if((hasTableSelection() && flags2.testFlag(opts2::ROUTE_CLEAR_SELECTION)) ||
       flags2.testFlag(opts2::ROUTE_CENTER_ACTIVE_LEG))
    {
#ifdef DEBUG_INFORMATION
      qDebug() << Q_FUNC_INFO << "tableCleanupTimer.start()";
#endif
      tableCleanupTimer.start();
    }
  }
}

void RouteController::viewScrolled(int)
{
  updateCleanupTimer();
}

void RouteController::sliderPressedOrReleased()
{
  updateCleanupTimer();
}

void RouteController::tableSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
  Q_UNUSED(selected)
  Q_UNUSED(deselected)

  // Get selected rows in ascending order
  selectedRows.clear();
  selectedRows = getSelectedRows(false);

  updateMoveAndDeleteActions();
  QItemSelectionModel *sm = view->selectionModel();

  if(sm == nullptr)
    return;

  int selectedRowSize = 0;
  if(sm != nullptr && sm->hasSelection())
    selectedRowSize = sm->selectedRows().size();

#ifdef DEBUG_INFORMATION
  if(sm != nullptr && sm->hasSelection())
  {
    int r = sm->currentIndex().row();
    if(r != -1)
      qDebug() << r << "#" << route.value(r);
  }
#endif

  NavApp::getMainUi()->pushButtonRouteClearSelection->setEnabled(sm != nullptr && sm->hasSelection());

  routeWindow->selectionChanged();

  emit routeSelectionChanged(selectedRowSize, model->rowCount());

  updateCleanupTimer();

  if(NavApp::getMainUi()->actionRouteFollowSelection->isChecked() && sm->currentIndex().isValid() &&
     sm->isSelected(sm->currentIndex()))
    emit showPos(route.value(sm->currentIndex().row()).getPosition(), map::INVALID_DISTANCE_VALUE, false);
}

/* Called by undo command */
void RouteController::changeRouteUndo(const atools::fs::pln::Flightplan& newFlightplan)
{
  // Keep our own index as a workaround
  undoIndex--;

  qDebug() << "changeRouteUndo undoIndex" << undoIndex << "undoIndexClean" << undoIndexClean;
  changeRouteUndoRedo(newFlightplan);
}

/* Called by undo command */
void RouteController::changeRouteRedo(const atools::fs::pln::Flightplan& newFlightplan)
{
  // Keep our own index as a workaround
  undoIndex++;
  qDebug() << "changeRouteRedo undoIndex" << undoIndex << "undoIndexClean" << undoIndexClean;
  changeRouteUndoRedo(newFlightplan);
}

/* Called by undo command when commands are merged */
void RouteController::undoMerge()
{
  undoIndex--;
#ifdef DEBUG_INFORMATION
  qDebug() << "undoMerge undoIndex" << undoIndex << "undoIndexClean" << undoIndexClean;
#endif
}

/* Update window after undo or redo action */
void RouteController::changeRouteUndoRedo(const atools::fs::pln::Flightplan& newFlightplan)
{
  route.clearAll();
  route.setFlightplan(newFlightplan);

  // Change format in plan according to last saved format
  route.createRouteLegsFromFlightplan();
  loadProceduresFromFlightplan(false /* clear old procedure properties */);
  loadAlternateFromFlightplan();
  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
  route.updateLegAltitudes();
  remarksFlightPlanToWidget();

  updateTableModel();
  updateMoveAndDeleteActions();
  emit routeChanged(true);
}

void RouteController::styleChanged()
{
  tabHandlerRoute->styleChanged();
  updateModelHighlights();
  highlightNextWaypoint(route.getActiveLegIndexCorrected());
}

void RouteController::optionsChanged()
{
  zoomHandler->zoomPercent(OptionData::instance().getGuiRouteTableTextSize());
  routeWindow->optionsChanged();

  tableCleanupTimer.setInterval(OptionData::instance().getSimCleanupTableTime() * 1000);

  updateTableHeaders();
  updateTableModel();

  updateUnits();
  view->update();

  updateCleanupTimer();
}

void RouteController::tracksChanged()
{
  postDatabaseLoad();
}

void RouteController::updateUnits()
{
  units->update();
}

bool RouteController::hasChanged() const
{
  return undoIndexClean == -1 || undoIndexClean != undoIndex;
}

float RouteController::getCruiseAltitudeWidget() const
{
  return Unit::rev(static_cast<float>(NavApp::getMainUi()->spinBoxRouteAlt->value()), Unit::altFeetF);
}

bool RouteController::isLnmFormatFlightplan()
{
  return route.getFlightplan().isLnmFormat();
}

bool RouteController::doesLnmFilenameMatchRoute()
{
  if(!routeFilename.isEmpty())
  {
    if(!(OptionData::instance().getFlags().testFlag(opts::GUI_AVOID_OVERWRITE_FLIGHTPLAN)))
      // User decided to ignore departure and destination change in flight plan
      return true;

    // Check if any parameters in the flight plan name have changed
    bool ok = true;
    const QString pattern = OptionData::instance().getFlightplanPattern();

    if(pattern.contains(atools::fs::pln::pattern::PLANTYPE))
      ok &= fileIfrVfr == route.getFlightplan().getFlightplanType();

    if(pattern.contains(atools::fs::pln::pattern::CRUISEALT))
      ok &= atools::almostEqual(fileCruiseAlt, route.getCruisingAltitudeFeet(), 10.f);

    if(pattern.contains(atools::fs::pln::pattern::DEPARTIDENT))
      ok &= fileDepartureIdent == route.getFlightplan().getDepartureIdent();

    if(pattern.contains(atools::fs::pln::pattern::DESTIDENT))
      ok &= fileDestinationIdent == route.getFlightplan().getDestinationIdent();

    return ok;
  }
  return false;
}

/* Called by action */
void RouteController::moveSelectedLegsDown()
{
  if(model->rowCount() <= 1)
    return;

  qDebug() << "Leg down";
  moveSelectedLegsInternal(MOVE_DOWN);
}

/* Called by action */
void RouteController::moveSelectedLegsUp()
{
  if(model->rowCount() <= 1)
    return;

  qDebug() << "Leg up";
  moveSelectedLegsInternal(MOVE_UP);
}

void RouteController::moveSelectedLegsInternal(MoveDirection direction)
{
  // Get the selected rows. Depending on move direction order can be reversed to ease moving
  QList<int> rows = getSelectedRows(direction == MOVE_DOWN /* reverse order */);

  if(!rows.isEmpty())
  {
    RouteCommand *undoCommand = preChange(tr("Move Waypoints"), rctype::MOVE);

    QModelIndex curIdx = view->currentIndex();
    // Remove selection
    if(view->selectionModel() != nullptr)
      view->selectionModel()->clear();
    for(int row : rows)
    {
      // Change flight plan
      route.getFlightplan().getEntries().move(row, row + direction);
      route.move(row, row + direction);

      // Move row
      model->insertRow(row + direction, model->takeRow(row));
    }

    int firstRow = rows.first();
    int lastRow = rows.last();

    bool forceDeparturePosition = false;
    if(direction == MOVE_DOWN)
    {
      qDebug() << "Move down" << firstRow << "to" << lastRow;
      // Departure moved down and was replaced by something else jumping up
      forceDeparturePosition = rows.contains(0);

      // Erase airway names at start of the moved block - last is smaller here
      route.eraseAirway(lastRow);
      route.eraseAirway(lastRow + 1);

      // Erase airway name at end of the moved block
      route.eraseAirway(firstRow + 2);
    }
    else if(direction == MOVE_UP)
    {
      qDebug() << "Move up" << firstRow << "to" << lastRow;
      // Something moved up and departure jumped up
      forceDeparturePosition = rows.contains(1);

      // Erase airway name at start of the moved block - last is larger here
      route.eraseAirway(firstRow - 1);

      // Erase airway names at end of the moved block
      route.eraseAirway(lastRow);
      route.eraseAirway(lastRow + 1);
    }

    route.updateAll();
    route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
    route.updateLegAltitudes();

    // Force update of start if departure airport was moved
    updateStartPositionBestRunway(forceDeparturePosition, false /* undo */);

    route.updateDepartureAndDestination();
    // Get type and cruise altitude from widgets
    updateFlightplanFromWidgets();

    updateActiveLeg();
    updateTableModel();

    // Restore current position at new moved position
    view->setCurrentIndex(model->index(curIdx.row() + direction, curIdx.column()));
    // Restore previous selection at new moved position
    selectList(rows, direction);

    updateMoveAndDeleteActions();

    postChange(undoCommand);
    emit routeChanged(true);
    NavApp::setStatusMessage(tr("Moved flight plan legs."));
  }
}

void RouteController::routeDelete(int index)
{
  deleteSelectedLegsInternal({index});
}

/* Called by action */
void RouteController::deleteSelectedLegs()
{
  deleteSelectedLegsInternal(getSelectedRows(true /* reverse */));
}

void RouteController::deleteSelectedLegsInternal(const QList<int>& rows)
{
  qDebug() << Q_FUNC_INFO << rows;

  if(!rows.isEmpty())
  {
    proc::MapProcedureTypes procs = affectedProcedures(rows);

    // Do not merge for procedure deletes
    RouteCommand *undoCommand = preChange(
      procs & proc::PROCEDURE_ALL ? tr("Delete Procedure") : tr("Delete Waypoints"),
      procs & proc::PROCEDURE_ALL ? rctype::EDIT : rctype::DELETE);

    int firstRow = rows.last();

    if(view->selectionModel() != nullptr)
      view->selectionModel()->clear();

    for(int row : rows)
    {
      route.getFlightplan().getEntries().removeAt(row);

      route.eraseAirway(row);

      route.removeAt(row);
      model->removeRow(row);
    }

    if(procs & proc::PROCEDURE_ALL)
    {
      // Remove dummy legs from flight plan and route
      route.removeProcedureLegs(procs);

      // Reload procedures from the database after deleting a transition.
      // This is needed since attached transitions can change procedures.
      route.reloadProcedures(procs);

      // Reload legs from procedures
      route.updateProcedureLegs(entryBuilder, true /* clear old procedure properties */, true /* cleanup route */);
    }

    route.updateIndicesAndOffsets();
    if(route.getSizeWithoutAlternates() == 0)
    {
      // Remove alternates too if last leg was deleted
      route.clear();
      route.getFlightplan().getEntries().clear();
    }

    route.updateAll();
    route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
    route.updateLegAltitudes();

    // Force update of start if departure airport was removed
    updateStartPositionBestRunway(rows.contains(0) /* force */, false /* undo */);

    route.updateDepartureAndDestination();

    // Get type and cruise altitude from widgets
    updateFlightplanFromWidgets();

    updateActiveLeg();
    updateTableModel();

    // Update current position at the beginning of the former selection
    view->setCurrentIndex(model->index(firstRow, 0));
    updateMoveAndDeleteActions();

    postChange(undoCommand);
    emit routeChanged(true);
    NavApp::setStatusMessage(tr("Removed flight plan legs."));
  }
}

/* Get selected row numbers from the table model */
QList<int> RouteController::getSelectedRows(bool reverse) const
{
  QList<int> rows;

  if(model->rowCount() == 0)
    return rows;

  if(view->selectionModel() != nullptr)
  {
    QItemSelection sm = view->selectionModel()->selection();
    for(const QItemSelectionRange& rng : sm)
    {
      for(int row = rng.top(); row <= rng.bottom(); row++)
        rows.append(row);
    }
  }

  if(!rows.isEmpty())
  {
    // Remove from bottom to top - otherwise model creates a mess
    std::sort(rows.begin(), rows.end());
    if(reverse)
      std::reverse(rows.begin(), rows.end());
  }

  // Remove duplicates
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  return rows;
}

/* Select all columns of the given rows adding offset to each row index */
void RouteController::selectList(const QList<int>& rows, int offset)
{
  // Check if model is already initailized
  if(model->rowCount() == 0)
    return;

  QItemSelection newSel;

  for(int row : rows)
    // Need to select all columns
    newSel.append(QItemSelectionRange(model->index(row + offset, rcol::FIRST_COLUMN),
                                      model->index(row + offset, rcol::LAST_COLUMN)));

  view->selectionModel()->select(newSel, QItemSelectionModel::ClearAndSelect);
}

void RouteController::selectRange(int from, int to)
{
  // Check if model is already initailized
  if(model->rowCount() == 0)
    return;

  QItemSelection newSel;

  int maxRows = view->model()->rowCount();

  if(from < 0 || to < 0 || from > maxRows - 1 || to > maxRows - 1)
    qWarning() << Q_FUNC_INFO << "not in range from" << from << "to" << to << ", min 0 max" << maxRows;

  from = std::min(std::max(from, 0), maxRows);
  to = std::min(std::max(to, 0), maxRows);

  newSel.append(QItemSelectionRange(model->index(from, rcol::FIRST_COLUMN),
                                    model->index(to, rcol::LAST_COLUMN)));

  view->selectionModel()->select(newSel, QItemSelectionModel::ClearAndSelect);
}

void RouteController::routeSetHelipad(const map::MapHelipad& helipad)
{
  qDebug() << Q_FUNC_INFO << helipad.id;

  map::MapStart start;
  airportQuery->getStartById(start, helipad.startId);

  routeSetStartPosition(start);
}

void RouteController::routeSetParking(const map::MapParking& parking)
{
  qDebug() << Q_FUNC_INFO << parking.id;

  RouteCommand *undoCommand = nullptr;

  // if(route.getFlightplan().canSaveDepartureParking())
  undoCommand = preChange(tr("Set Parking"));

  if(route.isEmpty() || route.getDepartureAirportLeg().getMapObjectType() != map::AIRPORT ||
     route.getDepartureAirportLeg().getId() != parking.airportId)
  {
    // No route, no start airport or different airport
    map::MapAirport ap;
    airportQuery->getAirportById(ap, parking.airportId);
    routeSetDepartureInternal(ap);
    route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);
  }

  // Update the current airport which is new or the same as the one used by the parking spot
  route.setDepartureParking(parking);
  route.updateAll();

  // Update flightplan copy
  pln::Flightplan& flightplan = route.getFlightplan();
  flightplan.setDepartureParkingName(map::parkingNameForFlightplan(parking));
  flightplan.setDepartureParkingType(atools::fs::pln::PARKING);
  flightplan.setDepartureParkingPosition(parking.position, route.getDepartureAirportLeg().getPosition().getAltitude(),
                                         parking.heading);

  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
  route.updateLegAltitudes();

  route.updateDepartureAndDestination();
  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();
  updateTableModel();
  updateMoveAndDeleteActions();

  postChange(undoCommand);
  emit routeChanged(true);

  NavApp::setStatusMessage(tr("Departure set to %1 parking %2.").arg(route.getDepartureAirportLeg().getDisplayIdent()).
                           arg(map::parkingNameNumber(parking)));
}

/* Set start position (runway, helipad) for departure */
void RouteController::routeSetStartPosition(map::MapStart start)
{
  qDebug() << "route set start id" << start.id;

  RouteCommand *undoCommand = preChange(tr("Set Start Position"));
  NavApp::showFlightPlan();

  if(route.isEmpty() || route.getDepartureAirportLeg().getMapObjectType() != map::AIRPORT ||
     route.getDepartureAirportLeg().getId() != start.airportId)
  {
    // No route, no start airport or different airport
    map::MapAirport ap;
    airportQuery->getAirportById(ap, start.airportId);
    routeSetDepartureInternal(ap);
    route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);
  }

  // Update the current airport which is new or the same as the one used by the parking spot
  route.setDepartureStart(start);
  route.updateAll();

  pln::Flightplan& flightplan = route.getFlightplan();
  flightplan.setDepartureParkingName(start.runwayName);
  if(route.hasDepartureRunway())
    flightplan.setDepartureParkingType(atools::fs::pln::RUNWAY);
  else if(route.hasDepartureHelipad())
    flightplan.setDepartureParkingType(atools::fs::pln::HELIPAD);
  else
    flightplan.setDepartureParkingType(atools::fs::pln::AIRPORT);

  flightplan.setDepartureParkingPosition(start.position,
                                         route.getDepartureAirportLeg().getPosition().getAltitude(),
                                         start.heading);

  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
  route.updateLegAltitudes();
  route.updateDepartureAndDestination();

  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();
  updateTableModel();
  updateMoveAndDeleteActions();

  postChange(undoCommand);
  emit routeChanged(true);

  NavApp::setStatusMessage(tr("Departure set to %1 start position %2.").
                           arg(route.getDepartureAirportLeg().getDisplayIdent()).
                           arg(start.runwayName));
}

void RouteController::routeSetDeparture(map::MapAirport airport)
{
  qDebug() << Q_FUNC_INFO << airport.id << airport.ident;

  if(!airport.isValid())
    return;

  RouteCommand *undoCommand = preChange(tr("Set Departure"));
  NavApp::showFlightPlan();

  routeSetDepartureInternal(airport);

  route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
  route.updateLegAltitudes();
  route.updateDepartureAndDestination();

  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  updateActiveLeg();
  updateTableModel();
  updateMoveAndDeleteActions();

  postChange(undoCommand);
  emit routeChanged(true);
  NavApp::setStatusMessage(tr("Departure set to %1.").arg(route.getDepartureAirportLeg().getDisplayIdent()));
}

/* Add departure and add best runway start position */
void RouteController::routeSetDepartureInternal(const map::MapAirport& airport)
{
  Flightplan& flightplan = route.getFlightplan();

  bool replaced = false;
  if(route.getSizeWithoutAlternates() > 1)
  {
    // Replace current departure
    const FlightplanEntry& first = flightplan.getEntries().first();
    if(first.getWaypointType() == pln::entry::AIRPORT &&
       flightplan.getDepartureIdent() == first.getIdent())
    {
      // Replace first airport
      FlightplanEntry entry;
      entryBuilder->buildFlightplanEntry(airport, entry, false /* alternate */);
      flightplan.getEntries().replace(0, entry);

      RouteLeg routeLeg(&flightplan);
      routeLeg.createFromAirport(0, airport, nullptr);
      route.replace(0, routeLeg);
      replaced = true;
    }
  }

  if(!replaced)
  {
    // Default is prepend
    FlightplanEntry entry;
    entryBuilder->buildFlightplanEntry(airport, entry, false /* alternate */);
    flightplan.getEntries().prepend(entry);

    RouteLeg routeLeg(&flightplan);
    routeLeg.createFromAirport(0, airport, nullptr);
    route.insert(0, routeLeg);
  }

  updateStartPositionBestRunway(true /* force */, false /* undo */);
}

void RouteController::routeSetDestination(map::MapAirport airport)
{
  qDebug() << Q_FUNC_INFO << airport.id << airport.ident;

  if(!airport.isValid())
    return;

  RouteCommand *undoCommand = preChange(tr("Set Destination"));
  NavApp::showFlightPlan();

  routeSetDestinationInternal(airport);

  route.removeProcedureLegs(proc::PROCEDURE_ARRIVAL_ALL);

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
  route.updateLegAltitudes();
  route.updateDepartureAndDestination();

  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  updateActiveLeg();
  updateTableModel();
  updateMoveAndDeleteActions();

  postChange(undoCommand);
  emit routeChanged(true);
  NavApp::setStatusMessage(tr("Destination set to %1.").arg(airport.ident));
}

void RouteController::routeAddAlternate(map::MapAirport airport)
{
  qDebug() << Q_FUNC_INFO << airport.id << airport.ident;

  if(!airport.isValid())
    return;

  RouteCommand *undoCommand = preChange(tr("Add Alternate"));
  NavApp::showFlightPlan();

  FlightplanEntry entry;
  entryBuilder->buildFlightplanEntry(airport, entry, true /* alternate */);

  Flightplan& flightplan = route.getFlightplan();
  flightplan.getEntries().append(entry);

  const RouteLeg *lastLeg = nullptr;
  if(flightplan.getEntries().size() > 1)
    // Set predecessor if route has entries
    lastLeg = &route.value(route.size() - 1);

  RouteLeg routeLeg(&flightplan);
  routeLeg.createFromAirport(flightplan.getEntries().size() - 1, airport, lastLeg);
  routeLeg.setAlternate();
  route.append(routeLeg);

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
  route.updateLegAltitudes();
  route.updateDepartureAndDestination();

  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  updateActiveLeg();
  updateTableModel();

  postChange(undoCommand);
  emit routeChanged(true);
  NavApp::setStatusMessage(tr("Alternate %1 added.").arg(airport.ident));
}

void RouteController::routeSetDestinationInternal(const map::MapAirport& airport)
{
  Flightplan& flightplan = route.getFlightplan();

  bool replaced = false;
  if(route.getSizeWithoutAlternates() > 1)
  {
    // Replace current destination
    int destIdx = route.getDestinationAirportLegIndex();
    if(destIdx != map::INVALID_INDEX_VALUE)
    {
      // Remove current destination
      const FlightplanEntry& last = flightplan.getEntries().at(destIdx);
      if(last.getWaypointType() == pln::entry::AIRPORT &&
         flightplan.getDestinationIdent() == last.getIdent())
      {
        // Replace destination airport
        FlightplanEntry entry;
        entryBuilder->buildFlightplanEntry(airport, entry, false /* alternate */);
        flightplan.getEntries().replace(destIdx, entry);

        RouteLeg routeLeg(&flightplan);
        const RouteLeg *lastLeg = destIdx > 1 ? &route.value(destIdx - 1) : nullptr;
        routeLeg.createFromAirport(destIdx, airport, lastLeg);
        route.replace(destIdx, routeLeg);
        replaced = true;
      }
    }
  }

  if(!replaced)
  {
    // Default is append
    int insertPos = route.size() - route.getNumAlternateLegs();

    FlightplanEntry entry;
    entryBuilder->buildFlightplanEntry(airport, entry, false /* alternate */);
    flightplan.getEntries().insert(insertPos, entry);

    RouteLeg routeLeg(&flightplan);
    const RouteLeg *lastLeg = insertPos > 1 ? &route.value(insertPos - 1) : nullptr;
    routeLeg.createFromAirport(insertPos, airport, lastLeg);
    route.insert(insertPos, routeLeg);
  }

  updateStartPositionBestRunway(false /* force */, false /* undo */);
}

void RouteController::showProceduresCustom(map::MapAirport airport)
{
  qDebug() << Q_FUNC_INFO << airport.id << airport.ident;

  CustomProcedureDialog dialog(mainWindow, airport);
  int result = dialog.exec();

  if(result == QDialog::Accepted)
  {
    map::MapRunway runway;
    map::MapRunwayEnd end;
    dialog.getSelected(runway, end);
    qDebug() << Q_FUNC_INFO << runway.primaryName << runway.secondaryName << end.id << end.name;

    proc::MapProcedureLegs procedure;
    NavApp::getProcedureQuery()->createCustomApproach(procedure, airport, end,
                                                      dialog.getEntryDistance(), dialog.getEntryAltitude());
    routeAddProcedure(procedure, QString());
  }
}

void RouteController::updateRouteTabChangedStatus()
{
  Ui::MainWindow *ui = NavApp::getMainUi();

  // Update status in flight plan table
  int idxRoute = NavApp::getRouteTabHandler()->getIndexForId(rc::ROUTE);
  if(idxRoute != -1)
  {
    if(hasChanged())
    {
      if(!ui->tabWidgetRoute->tabText(idxRoute).endsWith(tr(" *")))
        ui->tabWidgetRoute->setTabText(idxRoute, ui->tabWidgetRoute->tabText(idxRoute) + tr(" *"));
    }
    else
      ui->tabWidgetRoute->setTabText(idxRoute, ui->tabWidgetRoute->tabText(idxRoute).replace(tr(" *"), QString()));
  }

  // Update status in remarks tab
  int idxRemark = NavApp::getRouteTabHandler()->getIndexForId(rc::REMARKS);
  if(idxRemark != -1)
  {
    // Add star only if remarks contain text
    if(hasChanged() && !ui->plainTextEditRouteRemarks->toPlainText().isEmpty())
    {
      if(!ui->tabWidgetRoute->tabText(idxRemark).endsWith(tr(" *")))
        ui->tabWidgetRoute->setTabText(idxRemark, ui->tabWidgetRoute->tabText(idxRemark) + tr(" *"));
    }
    else
      ui->tabWidgetRoute->setTabText(idxRemark, ui->tabWidgetRoute->tabText(idxRemark).replace(tr(" *"), QString()));
  }
}

void RouteController::routeAddProcedure(proc::MapProcedureLegs legs, const QString& sidStarRunway)
{
  qDebug() << Q_FUNC_INFO
           << legs.approachType << legs.approachFixIdent << legs.approachSuffix << legs.approachArincName
           << legs.transitionType << legs.transitionFixIdent;

  if(legs.isEmpty())
  {
    qWarning() << Q_FUNC_INFO << "empty procedure";
    return;
  }

  RouteCommand *undoCommand = nullptr;

  // if(route.getFlightplan().canSaveProcedures())
  undoCommand = preChange(tr("Add Procedure"));

  if(route.isEmpty())
    NavApp::showFlightPlan();

  // Inserting new ones does not produce errors - only loading
  procedureErrors.clear();

  // Airport id in legs is from nav database - convert to simulator database
  map::MapAirport airportSim;
  if(legs.isCustom())
    NavApp::getAirportQuerySim()->getAirportById(airportSim, legs.ref.airportId);
  else
  {
    NavApp::getAirportQueryNav()->getAirportById(airportSim, legs.ref.airportId);
    mapQuery->getAirportSimReplace(airportSim);
  }

  if(legs.mapType & proc::PROCEDURE_STAR || legs.mapType & proc::PROCEDURE_ARRIVAL)
  {
    if(route.isEmpty() || route.getDestinationAirportLeg().getMapObjectType() != map::AIRPORT ||
       route.getDestinationAirportLeg().getId() != airportSim.id)
    {
      // No route, no destination airport or different airport
      route.removeProcedureLegs(proc::PROCEDURE_ARRIVAL_ALL);
      routeSetDestinationInternal(airportSim);
    }
    // Will take care of the flight plan entries too
    if(legs.mapType & proc::PROCEDURE_STAR)
    {
      // Assign runway for SID/STAR than can have multiple runways
      NavApp::getProcedureQuery()->insertSidStarRunway(legs, sidStarRunway);
      route.setStarProcedureLegs(legs);
    }
    if(legs.mapType & proc::PROCEDURE_ARRIVAL)
      route.setArrivalProcedureLegs(legs);

    route.updateProcedureLegs(entryBuilder, true /* clear old procedure properties */, true /* cleanup route */);
  }
  else if(legs.mapType & proc::PROCEDURE_DEPARTURE)
  {
    if(route.isEmpty() || route.getDepartureAirportLeg().getMapObjectType() != map::AIRPORT ||
       route.getDepartureAirportLeg().getId() != airportSim.id)
    {
      // No route, no departure airport or different airport
      route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);
      routeSetDepartureInternal(airportSim);
    }
    // Assign runway for SID/STAR than can have multiple runways
    NavApp::getProcedureQuery()->insertSidStarRunway(legs, sidStarRunway);

    route.setSidProcedureLegs(legs);

    // Will take care of the flight plan entries too
    route.updateProcedureLegs(entryBuilder, true /* clear old procedure properties */, true /* cleanup route */);
  }
  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
  route.updateLegAltitudes();
  route.updateDepartureAndDestination();

  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  updateActiveLeg();
  updateTableModel();
  updateMoveAndDeleteActions();

  postChange(undoCommand);

  qDebug() << Q_FUNC_INFO << route.getFlightplan().getProperties();

  emit routeChanged(true);

  NavApp::setStatusMessage(tr("Added procedure to flight plan."));
}

void RouteController::routeAdd(int id, atools::geo::Pos userPos, map::MapTypes type, int legIndex)
{
  qDebug() << Q_FUNC_INFO << "user pos" << userPos << "id" << id
           << "type" << type << "leg index" << legIndex;

  FlightplanEntry entry;
  entryBuilder->buildFlightplanEntry(id, userPos, type, entry, -1);

  int insertIndex = calculateInsertIndex(entry.getPosition(), legIndex);

  qDebug() << Q_FUNC_INFO << "insertIndex" << insertIndex;

  RouteCommand *undoCommand = preChange(tr("Add Waypoint"));

  if(route.isEmpty())
    NavApp::showFlightPlan();

  Flightplan& flightplan = route.getFlightplan();
  flightplan.getEntries().insert(insertIndex, entry);
  route.eraseAirway(insertIndex);
  route.eraseAirway(insertIndex + 1);

  const RouteLeg *lastLeg = nullptr;

  if(flightplan.isEmpty() && insertIndex > 0)
    lastLeg = &route.value(insertIndex - 1);
  RouteLeg routeLeg(&flightplan);
  routeLeg.createFromDatabaseByEntry(insertIndex, lastLeg);

  route.insert(insertIndex, routeLeg);

  proc::MapProcedureTypes procs = proc::PROCEDURE_NONE;

  if(legIndex == map::INVALID_INDEX_VALUE)
    procs = proc::PROCEDURE_ARRIVAL_ALL;
  else
    procs = affectedProcedures({insertIndex});

  route.removeProcedureLegs(procs);

  // Reload procedures from the database after deleting a transition.
  // This is needed since attached transitions can change procedures.
  route.reloadProcedures(procs);

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
  route.updateLegAltitudes();

  // Force update of start if departure airport was added
  updateStartPositionBestRunway(false /* force */, false /* undo */);
  route.updateDepartureAndDestination();
  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  updateActiveLeg();
  updateTableModel();
  updateMoveAndDeleteActions();

  postChange(undoCommand);

  emit routeChanged(true);

  NavApp::setStatusMessage(tr("Added waypoint to flight plan."));
}

void RouteController::routeReplace(int id, atools::geo::Pos userPos, map::MapTypes type,
                                   int legIndex)
{
  qDebug() << Q_FUNC_INFO << "user pos" << userPos << "id" << id
           << "type" << type << "leg index" << legIndex;
  const FlightplanEntry oldEntry = route.getFlightplan().getEntries().at(legIndex);
  bool alternate = oldEntry.getFlags() & atools::fs::pln::entry::ALTERNATE;

  if(alternate && !(type & map::AIRPORT))
    return;

  RouteCommand *undoCommand = preChange(tr("Change Waypoint"));

  FlightplanEntry entry;
  entryBuilder->buildFlightplanEntry(id, userPos, type, entry, -1);

  if(alternate)
    entry.setFlag(atools::fs::pln::entry::ALTERNATE);

  // Transfer user properties
  if(oldEntry.getWaypointType() == atools::fs::pln::entry::USER &&
     entry.getWaypointType() == atools::fs::pln::entry::USER)
  {
    entry.setIdent(oldEntry.getIdent());
    entry.setRegion(oldEntry.getRegion());
    entry.setName(oldEntry.getName());
    entry.setComment(oldEntry.getComment());
  }

  Flightplan& flightplan = route.getFlightplan();

  flightplan.getEntries().replace(legIndex, entry);

  const RouteLeg *lastLeg = nullptr;
  if(alternate)
    lastLeg = &route.getDestinationAirportLeg();
  else if(legIndex > 0 && !route.isFlightplanEmpty())
    // Get predecessor of replaced entry
    lastLeg = &route.value(legIndex - 1);

  RouteLeg routeLeg(&flightplan);
  routeLeg.createFromDatabaseByEntry(legIndex, lastLeg);

  route.replace(legIndex, routeLeg);
  route.eraseAirway(legIndex);
  route.eraseAirway(legIndex + 1);

  if(legIndex == route.getDestinationAirportLegIndex())
    route.removeProcedureLegs(proc::PROCEDURE_ARRIVAL_ALL);

  if(legIndex == 0)
    route.removeProcedureLegs(proc::PROCEDURE_DEPARTURE);

  route.updateAll();
  route.updateAirwaysAndAltitude(false /* adjustRouteAltitude */);
  route.updateLegAltitudes();

  // Force update of start if departure airport was changed
  updateStartPositionBestRunway(legIndex == 0 /* force */, false /* undo */);

  route.updateDepartureAndDestination();
  // Get type and cruise altitude from widgets
  updateFlightplanFromWidgets();

  updateActiveLeg();
  updateTableModel();
  updateMoveAndDeleteActions();

  postChange(undoCommand);
  emit routeChanged(true);
  NavApp::setStatusMessage(tr("Replaced waypoint in flight plan."));
}

int RouteController::calculateInsertIndex(const atools::geo::Pos& pos, int legIndex)
{
  Flightplan& flightplan = route.getFlightplan();

  int insertIndex = -1;
  if(legIndex == map::INVALID_INDEX_VALUE)
    // Append
    insertIndex = route.getSizeWithoutAlternates();
  else if(legIndex == -1)
  {
    if(flightplan.isEmpty())
      // First is  departure
      insertIndex = 0;
    else if(flightplan.getEntries().size() == 1)
      // Keep first as departure
      insertIndex = 1;
    else
    {
      // No leg index given - search for nearest available route leg
      atools::geo::LineDistance result;
      int nearestlegIndex = route.getNearestRouteLegResult(pos, result, true /* ignoreNotEditable */);

      switch(result.status)
      {
        case atools::geo::INVALID:
          insertIndex = 0;
          break;
        case atools::geo::ALONG_TRACK:
          insertIndex = nearestlegIndex;
          break;
        case atools::geo::BEFORE_START:
          if(nearestlegIndex == 1)
            // Add before departure
            insertIndex = 0;
          else
            insertIndex = nearestlegIndex;
          break;
        case atools::geo::AFTER_END:
          if(nearestlegIndex == route.getSizeWithoutAlternates() - 1)
            insertIndex = nearestlegIndex + 1;
          else
            insertIndex = nearestlegIndex;
          break;
      }
    }
  }
  else
    // Adjust and use given leg index (insert after index point)
    insertIndex = legIndex + 1;

  qDebug() << "insertIndex" << insertIndex << "pos" << pos;

  return insertIndex;
}

/* Update airway attribute in flight plan entry and return minimum altitude for this airway segment */
void RouteController::updateFlightplanEntryAirway(int airwayId, FlightplanEntry& entry)
{
  map::MapAirway airway;
  airwayQuery->getAirwayById(airway, airwayId);
  entry.setAirway(airway.name);
  entry.setFlag(atools::fs::pln::entry::TRACK, airway.isTrack());
}

/* Copy type and cruise altitude from widgets to flight plan */
void RouteController::updateFlightplanFromWidgets()
{
  assignFlightplanPerfProperties(route.getFlightplan());
  updateFlightplanFromWidgets(route.getFlightplan());
}

void RouteController::assignFlightplanPerfProperties(Flightplan& flightplan) const
{
  const atools::fs::perf::AircraftPerf perf = NavApp::getAircraftPerfController()->getAircraftPerformance();

  flightplan.getProperties().insert(atools::fs::pln::AIRCRAFT_PERF_NAME, perf.getName());
  flightplan.getProperties().insert(atools::fs::pln::AIRCRAFT_PERF_TYPE, perf.getAircraftType());
  flightplan.getProperties().insert(atools::fs::pln::AIRCRAFT_PERF_FILE,
                                    NavApp::getAircraftPerfController()->getCurrentFilepath());
}

void RouteController::updateFlightplanFromWidgets(Flightplan& flightplan)
{
  Ui::MainWindow *ui = NavApp::getMainUi();
  flightplan.setFlightplanType(ui->comboBoxRouteType->currentIndex() ==
                               0 ? atools::fs::pln::IFR : atools::fs::pln::VFR);
  flightplan.setCruisingAltitude(ui->spinBoxRouteAlt->value());
}

QIcon RouteController::iconForLeg(const RouteLeg& leg, int size) const
{
  QIcon icon;
  if(leg.getMapObjectType() == map::AIRPORT)
    icon = symbolPainter->createAirportIcon(leg.getAirport(), size - 2);
  else if(leg.getVor().isValid())
    icon = symbolPainter->createVorIcon(leg.getVor(), size);
  else if(leg.getNdb().isValid())
    icon = symbolPainter->createNdbIcon(size);
  else if(leg.getWaypoint().isValid())
    icon = symbolPainter->createWaypointIcon(size);
  else if(leg.getMapObjectType() == map::USERPOINTROUTE)
    icon = symbolPainter->createUserpointIcon(size);
  else if(leg.getMapObjectType() == map::INVALID)
    icon = symbolPainter->createWaypointIcon(size, mapcolors::routeInvalidPointColor);
  else if(leg.isAnyProcedure())
    icon = symbolPainter->createProcedurePointIcon(size);

  return icon;
}

void RouteController::updatePlaceholderWidget()
{
  Ui::MainWindow *ui = NavApp::getMainUi();

  bool showPlaceholder = route.isEmpty();
  ui->tableViewRoute->setVisible(!showPlaceholder);
  ui->textBrowserViewRoute->setVisible(showPlaceholder);
  ui->labelRouteInfo->setVisible(!showPlaceholder);
}

/* Update table view model completely */
void RouteController::updateTableModel()
{
  Ui::MainWindow *ui = NavApp::getMainUi();

  model->removeRows(0, model->rowCount());
  float totalDistance = route.getTotalDistance();

  int row = 0;
  float cumulatedDistance = 0.f;

  // Create null pointer filled list for all items ==============
  QList<QStandardItem *> itemRow;
  for(int i = rcol::FIRST_COLUMN; i <= rcol::LAST_COLUMN; i++)
    itemRow.append(nullptr);

  for(int i = 0; i < route.size(); i++)
  {
    const RouteLeg& leg = route.value(i);

    // Ident ===========================================
    QString identStr;
    if(leg.isAnyProcedure())
      // Get ident with IAF, FAF or other indication
      identStr = proc::procedureLegFixStr(leg.getProcedureLeg());
    else
      identStr = leg.getDisplayIdent();

    QStandardItem *ident =
      new QStandardItem(iconForLeg(leg, view->verticalHeader()->defaultSectionSize() - 2), identStr);
    QFont f = ident->font();
    f.setBold(true);
    ident->setFont(f);
    ident->setTextAlignment(Qt::AlignRight);

    itemRow[rcol::IDENT] = ident;
    // highlightProcedureItems() does error checking for IDENT

    // Region, navaid name, procedure type ===========================================
    itemRow[rcol::REGION] = new QStandardItem(leg.getRegion());
    itemRow[rcol::NAME] = new QStandardItem(leg.getName());

    if(row == route.getDepartureAirportLegIndex())
      itemRow[rcol::PROCEDURE] = new QStandardItem(tr("Departure"));
    else if(row == route.getDestinationAirportLegIndex())
      itemRow[rcol::PROCEDURE] = new QStandardItem(tr("Destination"));
    else if(leg.isAlternate())
      itemRow[rcol::PROCEDURE] = new QStandardItem(tr("Alternate"));
    else
      itemRow[rcol::PROCEDURE] = new QStandardItem(route.getProcedureLegText(leg.getProcedureType()));

    // Airway or leg type and restriction ===========================================
    if(leg.isRoute())
    {
      // Airway ========================
      const map::MapAirway& airway = leg.getAirway();

      QStringList awname;

      awname.append(airway.isValid() &&
                    airway.isTrack() ? tr("Track %1").arg(leg.getAirwayName()) : leg.getAirwayName());

      if(airway.isValid())
      {
        awname.append(map::airwayTrackTypeToShortString(airway.type));

#ifdef DEBUG_INFORMATION
        awname.append("[" + map::airwayRouteTypeToStringShort(airway.routeType) + "]");
#endif

        awname.removeAll(QString());
        itemRow[rcol::RESTRICTION] =
          new QStandardItem(map::airwayAltTextShort(airway, false /* addUnit */, false /* narrow */));
      }

      itemRow[rcol::AIRWAY_OR_LEGTYPE] = new QStandardItem(awname.join(tr(" / ")));
      // highlightProcedureItems() does error checking
    }
    else
    {
      // Procedure ========================
      itemRow[rcol::AIRWAY_OR_LEGTYPE] =
        new QStandardItem(atools::strJoin({leg.getFlightplanEntry().getAirway(),
                                           proc::procedureLegTypeStr(leg.getProcedureLegType())}, tr(",")));

      QString restrictions;
      if(leg.getProcedureLegAltRestr().isValid())
        restrictions.append(proc::altRestrictionTextShort(leg.getProcedureLegAltRestr()));
      if(leg.getProcedureLeg().speedRestriction.isValid())
        restrictions.append(tr("/") + proc::speedRestrictionTextShort(leg.getProcedureLeg().speedRestriction));

      itemRow[rcol::RESTRICTION] = new QStandardItem(restrictions);
    }

    // Get ILS for approach runway if it marks the end of an ILS or localizer approach procedure
    QStringList ilsTypeTexts, ilsFreqTexts;
    if(leg.getProcedureLeg().isApproach() && leg.getRunwayEnd().isValid())
    {
      // Build string for ILS type
      for(const map::MapIls& ils : route.getDestRunwayIlsRecommended())
      {
        ilsTypeTexts.append(map::ilsType(ils, true /* gs */, true /* dme */, tr("/")));
        ilsFreqTexts.append(ils.freqMHzOrChannelLocale());
      }
    }

    // VOR/NDB type ===========================
    if(leg.getVor().isValid())
      itemRow[rcol::TYPE] = new QStandardItem(map::vorFullShortText(leg.getVor()));
    else if(leg.getNdb().isValid())
      itemRow[rcol::TYPE] = new QStandardItem(map::ndbFullShortText(leg.getNdb()));
    else if(!ilsTypeTexts.isEmpty())
      itemRow[rcol::TYPE] = new QStandardItem(ilsTypeTexts.join(","));

    // VOR/NDB frequency =====================
    if(leg.getVor().isValid())
    {
      if(leg.getVor().tacan)
        itemRow[rcol::FREQ] = new QStandardItem(leg.getVor().channel);
      else
        itemRow[rcol::FREQ] = new QStandardItem(QLocale().toString(leg.getFrequency() / 1000.f, 'f', 2));
    }
    else if(leg.getNdb().isValid())
      itemRow[rcol::FREQ] = new QStandardItem(QLocale().toString(leg.getFrequency() / 100.f, 'f', 1));
    else if(!ilsFreqTexts.isEmpty())
      itemRow[rcol::FREQ] = new QStandardItem(ilsFreqTexts.join(","));

    // VOR/NDB range =====================
    if(leg.getRange() > 0 && (leg.getVor().isValid() || leg.getNdb().isValid()))
      itemRow[rcol::RANGE] = new QStandardItem(Unit::distNm(leg.getRange(), false));

    // Course =====================
    bool afterArrivalAirport = route.isAirportAfterArrival(i);
    if(row > 0 && !afterArrivalAirport && leg.getDistanceTo() < map::INVALID_DISTANCE_VALUE &&
       leg.getDistanceTo() > 0.f)
    {
      if(leg.getCourseToMag() < map::INVALID_COURSE_VALUE)
        itemRow[rcol::COURSE] = new QStandardItem(QLocale().toString(leg.getCourseToMag(), 'f', 0));
      if(leg.getCourseToTrue() < map::INVALID_COURSE_VALUE)
        itemRow[rcol::COURSETRUE] = new QStandardItem(QLocale().toString(leg.getCourseToTrue(), 'f', 0));
    }

    if(!afterArrivalAirport)
    {
      if(leg.getDistanceTo() < map::INVALID_DISTANCE_VALUE) // Distance =====================
      {
        cumulatedDistance += leg.getDistanceTo();
        itemRow[rcol::DIST] = new QStandardItem(Unit::distNm(leg.getDistanceTo(), false));

        if(!leg.getProcedureLeg().isMissed() && !leg.isAlternate())
        {
          float remaining = totalDistance - cumulatedDistance;
          if(remaining < 0.f)
            remaining = 0.f; // Catch the -0 case due to rounding errors
          itemRow[rcol::REMAINING_DISTANCE] = new QStandardItem(Unit::distNm(remaining, false));
        }
      }
    }

    itemRow[rcol::LATITUDE] = new QStandardItem(Unit::coordsLatY(leg.getPosition()));
    itemRow[rcol::LONGITUDE] = new QStandardItem(Unit::coordsLonX(leg.getPosition()));

    QString remarks;
    if(leg.isAnyProcedure())
      remarks = proc::procedureLegRemark(leg.getProcedureLeg());
    else
      remarks = leg.getFlightplanEntry().getComment();

    itemRow[rcol::REMARKS] = new QStandardItem(atools::elideTextShort(remarks, 80));
    itemRow[rcol::REMARKS]->setToolTip(atools::elideTextLinesShort(remarks, 20, 80));

    // Travel time, remaining fuel and ETA are updated in updateModelRouteTime

    // Create empty items for missing fields ===================
    for(int col = rcol::FIRST_COLUMN; col <= rcol::LAST_COLUMN; col++)
    {
      if(itemRow[col] == nullptr)
        itemRow[col] = new QStandardItem();

      // Do not allow editing and drag and drop
      itemRow[col]->setFlags(itemRow[col]->flags() &
                             ~(Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
    }

    // Align cells to the right - rest is aligned in updateModelRouteTimeFuel ===============
    itemRow[rcol::REGION]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::REMAINING_DISTANCE]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::DIST]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::COURSE]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::COURSETRUE]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::RANGE]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::FREQ]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::RESTRICTION]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::LEG_TIME]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::ETA]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::FUEL_WEIGHT]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::FUEL_VOLUME]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::WIND]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::WIND_HEAD_TAIL]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::ALTITUDE]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::LATITUDE]->setTextAlignment(Qt::AlignRight);
    itemRow[rcol::LONGITUDE]->setTextAlignment(Qt::AlignRight);

    model->appendRow(itemRow);

    for(int col = rcol::FIRST_COLUMN; col <= rcol::LAST_COLUMN; col++)
      itemRow[col] = nullptr;
    row++;
  }

  updateModelTimeFuelWind();

  Flightplan& flightplan = route.getFlightplan();

  if(!flightplan.isEmpty())
  {
    // Set spin box and block signals to avoid recursive call
    {
      QSignalBlocker blocker(ui->spinBoxRouteAlt);
      ui->spinBoxRouteAlt->setValue(flightplan.getCruisingAltitude());
    }

    { // Set combo box and block signals to avoid recursive call
      QSignalBlocker blocker(ui->comboBoxRouteType);
      if(flightplan.getFlightplanType() == atools::fs::pln::IFR)
        ui->comboBoxRouteType->setCurrentIndex(0);
      else if(flightplan.getFlightplanType() == atools::fs::pln::VFR)
        ui->comboBoxRouteType->setCurrentIndex(1);
    }
  }

  // Update header tooltips
  for(int col = rcol::FIRST_COLUMN; col <= rcol::LAST_COLUMN; col++)
    model->horizontalHeaderItem(col)->setToolTip(routeColumnTooltips.at(col));

  updateModelHighlights();
  highlightNextWaypoint(route.getActiveLegIndexCorrected());
  updateWindowLabel();
  updatePlaceholderWidget();

  // Value from ui file is ignored
  view->horizontalHeader()->setMinimumSectionSize(3);
}

/* Update travel times in table view model after speed change */
void RouteController::updateModelTimeFuelWind()
{
  // Check if model is already initailized
  if(model->rowCount() == 0)
    return;

  using atools::fs::perf::AircraftPerf;
  const RouteAltitude& altitudeLegs = route.getAltitudeLegs();
  if(altitudeLegs.isEmpty())
    return;

  int row = 0;
  float cumulatedTravelTime = 0.f;

  bool setValues = !altitudeLegs.hasErrors();
  const AircraftPerf& perf = NavApp::getAircraftPerformance();
  float totalFuelLbsOrGal = altitudeLegs.getTripFuel() + altitudeLegs.getAlternateFuel();

  if(setValues)
  {
    totalFuelLbsOrGal *= perf.getContingencyFuelFactor();
    totalFuelLbsOrGal += perf.getExtraFuel() + perf.getReserveFuel();
  }

  // Remember colum widths
  QHeaderView *header = view->horizontalHeader();
  int widthLegTime = header->isSectionHidden(rcol::LEG_TIME) ? -1 : view->columnWidth(rcol::LEG_TIME);
  int widthEta = header->isSectionHidden(rcol::ETA) ? -1 : view->columnWidth(rcol::ETA);
  int widthFuelWeight = header->isSectionHidden(rcol::FUEL_WEIGHT) ? -1 : view->columnWidth(rcol::FUEL_WEIGHT);
  int widthFuelVol = header->isSectionHidden(rcol::FUEL_VOLUME) ? -1 : view->columnWidth(rcol::FUEL_VOLUME);
  int widthWind = header->isSectionHidden(rcol::WIND) ? -1 : view->columnWidth(rcol::WIND);
  int widthWindHt = header->isSectionHidden(rcol::WIND_HEAD_TAIL) ? -1 : view->columnWidth(rcol::WIND_HEAD_TAIL);
  int widthAlt = header->isSectionHidden(rcol::ALTITUDE) ? -1 : view->columnWidth(rcol::ALTITUDE);

  for(int i = 0; i < route.size(); i++)
  {
    if(i >= model->rowCount())
    {
      qWarning() << Q_FUNC_INFO << "Route size exceeds model size" << i << model->rowCount() - 1;
      break;
    }

    if(!setValues)
    {
      // Do not fill if collecting performance or route altitude is invalid
      model->item(row, rcol::LEG_TIME)->setText(QString());
      model->item(row, rcol::ETA)->setText(QString());
      model->item(row, rcol::FUEL_WEIGHT)->setText(QString());
      model->item(row, rcol::FUEL_VOLUME)->setText(QString());
      model->item(row, rcol::WIND)->setText(QString());
      model->item(row, rcol::WIND_HEAD_TAIL)->setText(QString());
      model->item(row, rcol::ALTITUDE)->setText(QString());
    }
    else
    {
      const RouteLeg& leg = route.value(i);
      const RouteAltitudeLeg& altLeg = altitudeLegs.value(i);

      // Leg time =====================================================================
      float travelTime = altLeg.getTime();
      if(row == 0 || !(travelTime < map::INVALID_TIME_VALUE) || leg.getProcedureLeg().isMissed())
        model->item(row, rcol::LEG_TIME)->setText((QString()));
      else
      {
        QString txt = formatter::formatMinutesHours(travelTime);
#ifdef DEBUG_INFORMATION_LEGTIME
        txt += " [" + QString::number(travelTime * 3600., 'f', 0) + "]";
#endif
        model->item(row, rcol::LEG_TIME)->setText(txt);
      }

      if(!leg.getProcedureLeg().isMissed())
      {
        // Arrival time =====================================================================
        QString txt = formatter::formatMinutesHours(cumulatedTravelTime + travelTime);

        // Do not sum up for alternates - calculate again from destination for each alternate
        if(!leg.isAlternate())
          cumulatedTravelTime += travelTime;

#ifdef DEBUG_INFORMATION_LEGTIME
        txt += " [" + QString::number(cumulatedTravelTime * 3600., 'f', 0) + "]";
#endif
        model->item(row, rcol::ETA)->setText(txt);

        // Fuel at leg =====================================================================
        if(!leg.isAlternate())
          totalFuelLbsOrGal -= altLeg.getFuel();
        float totalTempFuel = totalFuelLbsOrGal;

        if(leg.isAlternate())
          totalTempFuel -= altLeg.getFuel();

        float weight = 0.f, vol = 0.f;
        if(perf.useFuelAsVolume())
        {
          weight = atools::geo::fromGalToLbs(perf.isJetFuel(), totalTempFuel);
          vol = totalTempFuel;
        }
        else
        {
          weight = totalTempFuel;
          vol = atools::geo::fromLbsToGal(perf.isJetFuel(), totalTempFuel);
        }

        if(atools::almostEqual(vol, 0.f, 0.01f))
          // Avoid -0 case
          vol = 0.f;
        if(atools::almostEqual(weight, 0.f, 0.01f))
          // Avoid -0 case
          weight = 0.f;

        txt = perf.isFuelFlowValid() ? Unit::weightLbs(weight, false /* addUnit */) : QString();
        model->item(row, rcol::FUEL_WEIGHT)->setText(txt);

        txt = perf.isFuelFlowValid() ? Unit::volGallon(vol, false /* addUnit */) : QString();
        model->item(row, rcol::FUEL_VOLUME)->setText(txt);

        // Wind at waypoint ========================================================
        // Exclude departure and all after including destination airport
        if(row > route.getDepartureAirportLegIndex() && row < route.getDestinationAirportLegIndex())
        {
          txt.clear();
          float headWind = 0.f, crossWind = 0.f;
          if(altLeg.getWindSpeed() >= 1.f)
          {
            atools::geo::windForCourse(headWind, crossWind, altLeg.getWindSpeed(),
                                       altLeg.getWindDirection(), leg.getCourseToTrue());

            txt = tr("%1 / %2").
                  arg(atools::geo::normalizeCourse(altLeg.getWindDirection() - leg.getMagvar()), 0, 'f', 0).
                  arg(Unit::speedKts(altLeg.getWindSpeed(), false /* addUnit */));
          }

          model->item(row, rcol::WIND)->setText(txt);

          // Head or tailwind at waypoint ========================================================
          txt.clear();
          if(std::abs(headWind) >= 1.f)
          {
            QString ptr;
            if(headWind >= 1.f)
              ptr = tr("▼");
            else if(headWind <= -1.f)
              ptr = tr("▲");
            txt.append(tr("%1 %2").arg(ptr).arg(Unit::speedKts(std::abs(headWind), false /* addUnit */)));
          }
          model->item(row, rcol::WIND_HEAD_TAIL)->setText(txt);
        }

        // Altitude at waypoint ========================================================
        float alt = altLeg.getWaypointAltitude();
        if(alt < map::INVALID_ALTITUDE_VALUE)
          txt = Unit::altFeet(alt, false /* addUnit */);
        model->item(row, rcol::ALTITUDE)->setText(txt);
      } // if(!leg.getProcedureLeg().isMissed())
    } // else if(!route.isAirportAfterArrival(row))
    row++;
  } // for(int i = 0; i < route.size(); i++)

  // Set back column widths if visible - widget changes widths on setItem
  if(widthLegTime > 0)
    view->setColumnWidth(rcol::LEG_TIME, widthLegTime);
  else
    header->hideSection(rcol::LEG_TIME);

  if(widthEta > 0)
    view->setColumnWidth(rcol::ETA, widthEta);
  else
    header->hideSection(rcol::ETA);

  if(widthFuelWeight > 0)
    view->setColumnWidth(rcol::FUEL_WEIGHT, widthFuelWeight);
  else
    header->hideSection(rcol::FUEL_WEIGHT);

  if(widthFuelVol > 0)
    view->setColumnWidth(rcol::FUEL_VOLUME, widthFuelVol);
  else
    header->hideSection(rcol::FUEL_VOLUME);

  if(widthWind > 0)
    view->setColumnWidth(rcol::WIND, widthWind);
  else
    header->hideSection(rcol::WIND);

  if(widthWindHt > 0)
    view->setColumnWidth(rcol::WIND_HEAD_TAIL, widthWindHt);
  else
    header->hideSection(rcol::WIND_HEAD_TAIL);

  if(widthAlt > 0)
    view->setColumnWidth(rcol::ALTITUDE, widthAlt);
  else
    header->hideSection(rcol::ALTITUDE);
}

void RouteController::disconnectedFromSimulator()
{
  qDebug() << Q_FUNC_INFO;

  route.resetActive();
  highlightNextWaypoint(-1);
  emit routeChanged(false);
}

void RouteController::simDataChanged(const atools::fs::sc::SimConnectData& simulatorData)
{
  if(!loadingDatabaseState && atools::almostNotEqual(QDateTime::currentDateTime().toMSecsSinceEpoch(),
                                                     lastSimUpdate, static_cast<qint64>(MIN_SIM_UPDATE_TIME_MS)))
  {
    if(simulatorData.isUserAircraftValid())
    {
      aircraft = simulatorData.getUserAircraftConst();

      // Sequence only for airborne airplanes
      // Use more than one parameter since first X-Plane data packets are unreliable
      map::PosCourse position(aircraft.getPosition(), aircraft.getTrackDegTrue());
      if(aircraft.isFlying())
      {
        int previousRouteLeg = route.getActiveLegIndexCorrected();
        route.updateActiveLegAndPos(position);
        int activeLegIdx = route.getActiveLegIndexCorrected();

        if(!tableCleanupTimer.isActive())
        {
          // Timer is not active - set active if any option is set
          if((hasTableSelection() && OptionData::instance().getFlags2().testFlag(opts2::ROUTE_CLEAR_SELECTION)) ||
             OptionData::instance().getFlags2().testFlag(opts2::ROUTE_CENTER_ACTIVE_LEG))
            tableCleanupTimer.start();
        }

        if(activeLegIdx != previousRouteLeg)
        {
          // Use corrected indexes to highlight initial fix
          qDebug() << "new route leg" << previousRouteLeg << activeLegIdx;
          highlightNextWaypoint(activeLegIdx);
          NavApp::updateAllMaps();
        }
      }
      else
        route.updateActivePos(position);
    }
    lastSimUpdate = QDateTime::currentDateTime().toMSecsSinceEpoch();
  }
}

void RouteController::scrollToActive()
{
  if(NavApp::isConnectedAndAircraftFlying())
  {
    int routeLeg = route.getActiveLegIndexCorrected();

    // Check if model is already initailized
    if(model->rowCount() == 0)
      return;

    if(routeLeg != map::INVALID_INDEX_VALUE && routeLeg >= 0 &&
       OptionData::instance().getFlags2().testFlag(opts2::ROUTE_CENTER_ACTIVE_LEG))
      view->scrollTo(model->index(std::max(routeLeg - 1, 0), 0), QAbstractItemView::PositionAtTop);
  }
}

/* */
void RouteController::highlightNextWaypoint(int activeLegIdx)
{
  // Check if model is already initailized
  if(model->rowCount() == 0)
    return;

  // Remove background brush for all rows and columns ===============
  activeLegIndex = activeLegIdx;
  for(int row = 0; row < model->rowCount(); ++row)
  {
    for(int col = 0; col < model->columnCount(); col++)
    {
      QStandardItem *item = model->item(row, col);
      if(item != nullptr)
      {
        item->setBackground(Qt::NoBrush);
        // Keep first column bold
        if(item->font().bold() && col != 0)
        {
          QFont font = item->font();
          font.setBold(false);
          item->setFont(font);
        }
      }
    }
  }

  if(!route.isEmpty())
  {
    // Add magenta brush for all columns in active row ======================
    if(activeLegIndex >= 0 && activeLegIndex < route.size())
    {
      QColor color = NavApp::isCurrentGuiStyleNight() ?
                     mapcolors::nextWaypointColorDark : mapcolors::nextWaypointColor;

      for(int col = 0; col < model->columnCount(); col++)
      {
        QStandardItem *item = model->item(activeLegIndex, col);
        if(item != nullptr)
        {
          item->setBackground(color);
          if(!item->font().bold())
          {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
          }
        }
      }
    }
  }
}

/* Set colors for procedures and missing objects like waypoints and airways */
void RouteController::updateModelHighlights()
{
  // Check if model is already initailized
  if(model->rowCount() == 0)
    return;

  bool night = NavApp::isCurrentGuiStyleNight();
  const QColor defaultColor = QApplication::palette().color(QPalette::Normal, QPalette::Text);
  const QColor invalidColor = night ? mapcolors::routeInvalidTableColorDark : mapcolors::routeInvalidTableColor;
  flightplanErrors.clear();
  trackErrors = false;

  for(int row = 0; row < model->rowCount(); row++)
  {
    const RouteLeg& leg = route.value(row);
    if(!leg.isValid())
    {
      // Have to check here since sim updates can still happen while building the flight plan
      qWarning() << Q_FUNC_INFO << "Invalid index" << row;
      break;
    }

    for(int col = 0; col < model->columnCount(); col++)
    {
      QStandardItem *item = model->item(row, col);
      if(item != nullptr)
      {
        // Set default font color for all items ==============
        item->setForeground(defaultColor);

        if(leg.isAlternate())
          item->setForeground(night ? mapcolors::routeAlternateTableColorDark : mapcolors::routeAlternateTableColor);
        else if(leg.isAnyProcedure())
        {
          if(leg.getProcedureLeg().isMissed())
            item->setForeground(
              night ? mapcolors::routeProcedureMissedTableColorDark : mapcolors::routeProcedureMissedTableColor);
          else
            item->setForeground(night ? mapcolors::routeProcedureTableColorDark : mapcolors::routeProcedureTableColor);
        }

        // Ident colum ==========================================
        if(col == rcol::IDENT)
        {
          if(leg.getMapObjectType() == map::INVALID)
          {
            item->setForeground(invalidColor);
            QString err = tr("Waypoint \"%1\" not found.").arg(leg.getDisplayIdent());
            item->setToolTip(err);
            flightplanErrors.append(err);
          }
          else
            item->setToolTip(QString());

          if(leg.getAirport().isValid())
          {
            QFont font = item->font();
            if(leg.getAirport().addon())
            {
              font.setItalic(true);
              font.setUnderline(true);
            }
            if(leg.getAirport().closed())
              font.setStrikeOut(true);
            item->setFont(font);
          }
        }

        // Airway colum ==========================================
        if(col == rcol::AIRWAY_OR_LEGTYPE && leg.isRoute())
        {
          QStringList airwayErrors;
          bool trackError = false;
          if(leg.isAirwaySetAndInvalid(route.getCruisingAltitudeFeet(), &airwayErrors, &trackError))
          {
            // Has airway but errors
            item->setForeground(invalidColor);
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
            if(!airwayErrors.isEmpty())
            {
              item->setToolTip(airwayErrors.join(tr("\n")));
              flightplanErrors.append(airwayErrors);
            }
          }
          else if(row != activeLegIndex)
          {
            // No airway or no errors - leave font bold if this is the active
            QFont font = item->font();
            font.setBold(false);
            item->setFont(font);
            item->setToolTip(QString());
          }

          trackErrors |= trackError;
        }
      }
    }
  }
}

bool RouteController::hasErrors() const
{
  return !flightplanErrors.isEmpty() || !procedureErrors.isEmpty() || !alternateErrors.isEmpty();
}

QString RouteController::getErrorStrings(QStringList& toolTip) const
{
  if(hasErrors())
  {
    if(!flightplanErrors.isEmpty())
      toolTip.append(flightplanErrors);

    if(!procedureErrors.isEmpty())
      toolTip.append(tr("Cannot load %1: %2").
                     arg(procedureErrors.size() > 1 ? tr("procedures") : tr("procedure")).
                     arg(procedureErrors.join(tr(", "))));

    if(!alternateErrors.isEmpty())
      toolTip.append(tr("Cannot load %1: %2").
                     arg(alternateErrors.size() > 1 ? tr("alternates") : tr("alternate")).
                     arg(alternateErrors.join(tr(", "))));

    if(trackErrors)
      toolTip.append(tr("Download oceanic tracks in menu \"Flight Plan\"\n"
                        "or calculate the flight plan again if your plan uses tracks.",
                        "Keep in sync with menu names"));

    return tr("Errors in flight plan.");
  }
  else
    return QString();
}

/* Update the dock window top level label */
void RouteController::updateWindowLabel()
{
  QString text =
    buildFlightplanLabel(false /* print */, true /* widget */, false /* titleOnly */) +
    "<br/>" +
    buildFlightplanLabel2();

  Ui::MainWindow *ui = NavApp::getMainUi();
  ui->labelRouteInfo->setText(text);
}

QString RouteController::buildFlightplanLabel(bool print, bool widget, bool titleOnly) const
{
  const Flightplan& flightplan = route.getFlightplan();

  QString departureAirport(tr("Invalid")), departureParking, destinationAirport(tr("Invalid")), approach;

  if(!flightplan.isEmpty())
  {
    QString starRunway, approachRunway;

    // Add departure to text ==============================================================
    if(route.hasValidDeparture())
    {
      departureAirport = tr("%1 (%2)").
                         arg(route.getDepartureAirportLeg().getName()).
                         arg(route.getDepartureAirportLeg().getDisplayIdent());

      if(route.getDepartureAirportLeg().getDepartureParking().isValid())
        departureParking = tr(" %1").arg(map::parkingNameNumber(route.getDepartureAirportLeg().getDepartureParking()));
      else if(route.getDepartureAirportLeg().getDepartureStart().isValid())
      {
        const map::MapStart& start = route.getDepartureAirportLeg().getDepartureStart();
        if(route.hasDepartureHelipad())
          departureParking += tr(" Helipad %1").arg(start.runwayName);
        else if(route.hasDepartureRunway())
          departureParking += tr(" Runway %1").arg(start.runwayName);
        else
          departureParking += tr(" Unknown Start");
      }
    }
    else
    {
      departureAirport = tr("%1 (%2)").
                         arg(flightplan.getEntries().first().getIdent()).
                         arg(flightplan.getEntries().first().getWaypointTypeAsDisplayString());
    }

    // Add destination to text ==============================================================
    if(route.hasValidDestination())
      destinationAirport = tr("%1 (%2)").
                           arg(route.getDestinationAirportLeg().getName()).
                           arg(route.getDestinationAirportLeg().getDisplayIdent());
    else
      destinationAirport = tr("%1 (%2)").
                           arg(flightplan.at(route.getDestinationAirportLegIndex()).getIdent()).
                           arg(flightplan.at(route.getDestinationAirportLegIndex()).getWaypointTypeAsDisplayString());

    if(!titleOnly)
    {
      // Add procedures to text ==============================================================
      const proc::MapProcedureLegs& arrivalLegs = route.getApproachLegs();
      const proc::MapProcedureLegs& starLegs = route.getStarLegs();
      if(route.hasAnyProcedure())
      {
        QStringList procedureText;
        QVector<bool> boldTextFlag;

        const proc::MapProcedureLegs& departureLegs = route.getSidLegs();
        if(!departureLegs.isEmpty())
        {
          // Add departure procedure to text
          if(!departureLegs.runwayEnd.isValid())
          {
            boldTextFlag << false;
            procedureText.append(tr("Depart via SID"));
          }
          else
          {
            boldTextFlag << false << true << false;
            procedureText.append(tr("Depart runway"));
            procedureText.append(departureLegs.runwayEnd.name);
            procedureText.append(tr("via SID"));
          }

          QString sid(departureLegs.approachFixIdent);
          if(!departureLegs.transitionFixIdent.isEmpty())
            sid += "." + departureLegs.transitionFixIdent;
          boldTextFlag << true;
          procedureText.append(sid);

          if(arrivalLegs.mapType & proc::PROCEDURE_ARRIVAL_ALL || starLegs.mapType & proc::PROCEDURE_ARRIVAL_ALL)
          {
            boldTextFlag << false;
            procedureText.append(tr("."));
          }
        }

        // Add arrival procedures procedure to text
        // STAR
        if(!starLegs.isEmpty())
        {
          if(print)
          {
            // Add line break between departure and arrival for printing
            boldTextFlag << false;
            procedureText.append("<br/>");
          }

          boldTextFlag << false << true;
          procedureText.append(tr("Arrive via STAR"));

          QString star(starLegs.approachFixIdent);
          if(!starLegs.transitionFixIdent.isEmpty())
            star += "." + starLegs.transitionFixIdent;
          procedureText.append(star);

          starRunway = starLegs.procedureRunway;

          if(!(arrivalLegs.mapType & proc::PROCEDURE_APPROACH))
          {
            boldTextFlag << false << true;
            procedureText.append(tr("at runway"));
            procedureText.append(starLegs.procedureRunway);
          }
          else if(!starLegs.procedureRunway.isEmpty())
          {
            boldTextFlag << false;
            procedureText.append(tr("(<b>%1</b>)").arg(starLegs.procedureRunway));
          }

          if(!(arrivalLegs.mapType & proc::PROCEDURE_APPROACH))
          {
            boldTextFlag << false;
            procedureText.append(tr("."));
          }
        }

        if(arrivalLegs.mapType & proc::PROCEDURE_TRANSITION)
        {
          boldTextFlag << false << true;
          procedureText.append(!starLegs.isEmpty() ? tr("via") : tr("Via"));
          procedureText.append(arrivalLegs.transitionFixIdent);
        }

        if(arrivalLegs.mapType & proc::PROCEDURE_APPROACH)
        {
          boldTextFlag << false;
          procedureText.append((arrivalLegs.mapType & proc::PROCEDURE_TRANSITION ||
                                !starLegs.isEmpty()) ? tr("and") : tr("Via"));

          // Type and suffix =======================
          QString type(arrivalLegs.displayApproachType());
          if(!arrivalLegs.approachSuffix.isEmpty())
            type += tr("-%1").arg(arrivalLegs.approachSuffix);

          boldTextFlag << true;
          procedureText.append(type);

          boldTextFlag << true;
          procedureText.append(arrivalLegs.approachFixIdent);

          if(!arrivalLegs.approachArincName.isEmpty())
          {
            boldTextFlag << true;
            procedureText.append(tr("(%1)").arg(arrivalLegs.approachArincName));
          }

          // Runway =======================
          if(arrivalLegs.runwayEnd.isValid() && !arrivalLegs.runwayEnd.name.isEmpty())
          {
            // Add runway for approach
            boldTextFlag << false << true << false;
            procedureText.append(procedureText.isEmpty() ? tr("To runway") : tr("to runway"));
            procedureText.append(arrivalLegs.runwayEnd.name);
            procedureText.append(tr("."));
          }
          else
          {
            // Add runway text
            boldTextFlag << false;
            procedureText.append(procedureText.isEmpty() ? tr("To runway.") : tr("to runway."));
          }
          approachRunway = arrivalLegs.runwayEnd.name;
        }

        if(!approachRunway.isEmpty() && !starRunway.isEmpty() &&
           !atools::fs::util::runwayEqual(approachRunway, starRunway))
        {
          boldTextFlag << true;
          procedureText.append(
            atools::util::HtmlBuilder::errorMessage(tr("Runway mismatch: STAR \"%1\" ≠ Approach \"%2\".").
                                                    arg(starRunway).arg(approachRunway)));
        }

        for(int i = 0; i < procedureText.size(); i++)
        {
          if(boldTextFlag.at(i))
            procedureText[i] = tr("<b>") + procedureText.at(i) + tr("</b>");
        }
        approach = procedureText.join(" ");
      }
    }
  }

  QString title;
  if(!flightplan.isEmpty())
  {
    if(print)
      // Printer
      title = tr("<h2>%1%2 to %3</h2>").arg(departureAirport).arg(departureParking).arg(destinationAirport);
    else if(widget)
      // Table header
      title = tr("<b><a style=\"text-decoration:none;\" href=\"lnm://showdeparture\">%1</a>"
                   "<a style=\"text-decoration:none;\" href=\"lnm://showdepartureparking\">%2</a></b> to "
                     "<b><a style=\"text-decoration:none;\" href=\"lnm://showdestination\">%3</a></b>").
              arg(departureAirport).arg(departureParking).arg(destinationAirport);
    else
      // HTML export
      title = tr("<b>%1%2</b> to <b>%3</b>").arg(departureAirport).arg(departureParking).arg(destinationAirport);
  }

  if(print)
    return title + (approach.isEmpty() ? QString() : "<p><big>" + approach + "</big></p>");
  else
    return title + (approach.isEmpty() ? QString() : "<br/>" + approach);
}

QString RouteController::buildFlightplanLabel2(bool print) const
{
  const Flightplan& flightplan = route.getFlightplan();
  if(!flightplan.isEmpty())
  {
    QStringList texts;

    if(route.getSizeWithoutAlternates() > 1)
      texts.append(tr("<b>%1</b>").arg(Unit::distNm(route.getTotalDistance())));

    if(route.getAltitudeLegs().getTravelTimeHours() > 0.f)
      texts.append(tr("<b>%1</b>").arg(formatter::formatMinutesHoursLong(route.getAltitudeLegs().getTravelTimeHours())));

    if(print)
    {
      texts.append(tr("<b>%1</b>").arg(Unit::altFeet(route.getCruisingAltitudeFeet())));
      if(route.getTopOfClimbDistance() < map::INVALID_DISTANCE_VALUE)
        texts.append(tr("%1 from departure to top of climb").arg(Unit::distNm(route.getTopOfClimbDistance())));
      if(route.getTopOfDescentFromDestination() < map::INVALID_DISTANCE_VALUE)
        texts.append(tr("%1 from start of descent to destination").arg(Unit::distNm(
                                                                         route.getTopOfDescentFromDestination())));
    }

    return texts.join(tr(", "));
  }
  else
    return QString();
}

void RouteController::flightplanLabelLinkActivated(const QString& link)
{
  qDebug() << Q_FUNC_INFO << link;
  // lnm://showdeparture

  QUrl url(link);
  if(url.scheme() == "lnm")
  {
    if(url.host() == "showdeparture")
      showAtIndex(route.getDepartureAirportLegIndex(), true /* info */, true /* map */, false /* doubleClick */);
    else if(url.host() == "showdepartureparking")
    {
      const RouteLeg& departureAirportLeg = route.getDepartureAirportLeg();
      if(departureAirportLeg.getDepartureParking().isValid())
        emit showPos(departureAirportLeg.getDepartureParking().getPosition(), 0.f, false /* doubleClick */);
      else if(departureAirportLeg.getDepartureStart().isValid())
        emit showPos(departureAirportLeg.getDepartureStart().getPosition(), 0.f, false /* doubleClick */);
      showAtIndex(route.getDepartureAirportLegIndex(), true /* info */, false /* map */, false /* doubleClick */);
    }
    else if(url.host() == "showdestination")
      showAtIndex(route.getDestinationAirportLegIndex(), true /* info */, true /* map */, false /* doubleClick */);
  }
}

/* Reset route and clear undo stack (new route) */
void RouteController::clearRoute()
{
  route.clearAll();
  routeFilename.clear();
  fileDepartureIdent.clear();
  fileDestinationIdent.clear();
  fileIfrVfr = pln::VFR;
  fileCruiseAlt = 0.f;
  undoStack->clear();
  undoIndex = 0;
  undoIndexClean = 0;
  entryBuilder->setCurUserpointNumber(1);
  updateFlightplanFromWidgets();
}

/* Call this before doing any change to the flight plan that should be undoable */
RouteCommand *RouteController::preChange(const QString& text, rctype::RouteCmdType rcType)
{
  // Clean the flight plan from any procedure entries
  Flightplan flightplan = route.getFlightplan();
  flightplan.removeNoSaveEntries();
  return new RouteCommand(this, flightplan, text, rcType);
}

/* Call this after doing a change to the flight plan that should be undoable */
void RouteController::postChange(RouteCommand *undoCommand)
{
  if(undoCommand == nullptr)
    return;

  // Clean the flight plan from any procedure entries
  Flightplan flightplan = route.getFlightplan();
  flightplan.removeNoSaveEntries();
  undoCommand->setFlightplanAfter(flightplan);

  if(undoIndex < undoIndexClean)
    undoIndexClean = -1;

  // Index and clean index workaround
  undoIndex++;
#ifdef DEBUG_INFORMATION
  qDebug() << "postChange undoIndex" << undoIndex << "undoIndexClean" << undoIndexClean;
#endif
  undoStack->push(undoCommand);
}

/*
 * Select the best runway start position for the departure airport.
 * @param force Update even if a start position is already set
 * @param undo Embed in undo operation
 * @return true if parking was changed
 */
bool RouteController::updateStartPositionBestRunway(bool force, bool undo)
{
  if(route.hasValidDeparture())
  {
    if(force || (!route.hasDepartureParking() && !route.hasDepartureStart()))
    {
      QString dep, arr;
      route.getRunwayNames(dep, arr);

      // Reset departure position to best runway
      map::MapStart start;
      airportQuery->getBestStartPositionForAirport(start, route.getDepartureAirportLeg().getAirport().id, dep);

      // Check if the airport has a start position - sone add-on airports don't
      if(start.isValid())
      {
        RouteCommand *undoCommand = nullptr;

        if(undo)
          undoCommand = preChange(tr("Set Start Position"));

        route.setDepartureStart(start);
        route.updateDepartureAndDestination();

        if(undo)
          postChange(undoCommand);
        return true;
      }
    }
  }
  return false;
}

proc::MapProcedureTypes RouteController::affectedProcedures(const QList<int>& indexes)
{
  qDebug() << Q_FUNC_INFO << indexes;
  proc::MapProcedureTypes types = proc::PROCEDURE_NONE;

  for(int index : indexes)
  {
    if(index == 0)
      // Delete SID if departure airport is affected
      types |= proc::PROCEDURE_DEPARTURE;

    if(index >= route.getDestinationAirportLegIndex())
    {
      int altIndex = route.getAlternateLegsOffset();
      // Check if trying to delete an alternate
      if(altIndex == map::INVALID_INDEX_VALUE || index < altIndex)
        // Delete all arrival procedures if destination airport is affected or an new leg is appended after
        types |= proc::PROCEDURE_ARRIVAL_ALL;
    }

    if(index >= 0 && index < route.getDestinationAirportLegIndex())
    {
      const proc::MapProcedureLeg& leg = route.value(index).getProcedureLeg();

      if(leg.isSidTransition())
        types |= proc::PROCEDURE_SID_TRANSITION;

      if(leg.isSid())
        // Delete SID and transition
        types |= proc::PROCEDURE_DEPARTURE;

      if(leg.isStarTransition())
        types |= proc::PROCEDURE_STAR_TRANSITION;

      if(leg.isStar())
        // Delete STAR and transition
        types |= proc::PROCEDURE_STAR_ALL;

      if(leg.isTransition())
        // Delete transition only
        types |= proc::PROCEDURE_TRANSITION;

      if(leg.isApproach() || leg.isMissed())
        // Delete transition and approach
        types |= proc::PROCEDURE_ARRIVAL;
    }
  }

  if(types & proc::PROCEDURE_SID_TRANSITION && route.getSidLegs().approachLegs.isEmpty() &&
     !route.getSidLegs().approachFixIdent.isEmpty())
    // Remove the empty SID structure too
    types |= proc::PROCEDURE_SID;

  if(types & proc::PROCEDURE_STAR_TRANSITION && route.getStarLegs().approachLegs.isEmpty())
    // Remove the empty STAR structure too
    types |= proc::PROCEDURE_STAR_ALL;

  return types;
}

void RouteController::remarksFlightPlanToWidget()
{
  Ui::MainWindow *ui = NavApp::getMainUi();

  // Block signals to avoid recursion
  ui->plainTextEditRouteRemarks->blockSignals(true);
  ui->plainTextEditRouteRemarks->setPlainText(route.getFlightplan().getComment());
  ui->plainTextEditRouteRemarks->blockSignals(false);
}

void RouteController::remarksTextChanged()
{
  if(route.isEmpty())
    return;

  QString editText = NavApp::getMainUi()->plainTextEditRouteRemarks->toPlainText();
  pln::Flightplan& flightplan = route.getFlightplan();

  if(flightplan.getComment() != editText)
  {
    // Text is different add with undo
    RouteCommand *undoCommand = preChange(tr("Remarks changed"), rctype::REMARKS);
    flightplan.setComment(editText);
    postChange(undoCommand);

    // Update title and tab with "*" for changed
    NavApp::updateWindowTitle();
  }
}

void RouteController::updateRemarkWidget()
{
  if(route.isEmpty())
  {
    NavApp::getMainUi()->plainTextEditRouteRemarks->setDisabled(true);
    NavApp::getMainUi()->plainTextEditRouteRemarks->setPlaceholderText(tr("No flight plan.") +
                                                                       tr("\n\n") +
                                                                       tr("Remarks for the flight plan."));
  }
  else
  {
    NavApp::getMainUi()->plainTextEditRouteRemarks->setDisabled(false);
    NavApp::getMainUi()->plainTextEditRouteRemarks->setPlaceholderText(tr("Remarks for the flight plan."));
  }
}

QStringList RouteController::getAllRouteColumns() const
{
  QStringList colums;
  for(int i = rcol::FIRST_COLUMN; i <= rcol::LAST_COLUMN; i++)
    colums.append(Unit::replacePlaceholders(routeColumns.value(i)).replace("-\n", "").replace("\n", " "));

  return colums;
}

void RouteController::clearAllErrors()
{
  procedureErrors.clear();
  alternateErrors.clear();
  flightplanErrors.clear();
  trackErrors = false;
}

#ifdef DEBUG_NETWORK_INFORMATION

void RouteController::debugNetworkClick(const atools::geo::Pos& pos)
{
  if(pos.isValid())
  {
    qDebug() << Q_FUNC_INFO << pos;

    atools::routing::RouteNetworkLoader loader(NavApp::getDatabaseNav(), NavApp::getDatabaseTrack());
    if(!routeNetworkAirway->isLoaded())
      loader.load(routeNetworkAirway);
    if(!routeNetworkRadio->isLoaded())
      loader.load(routeNetworkRadio);

    atools::routing::Node node = routeNetworkAirway->getNearestNode(pos);
    if(node.isValid())
    {
      qDebug() << "Airway node" << node;
      qDebug() << "Airway edges" << node.edges;
    }

    node = routeNetworkRadio->getNearestNode(pos);
    if(node.isValid())
    {
      qDebug() << "Radio node" << node;
      qDebug() << "Radio edges" << node.edges;
    }
  }
}

#endif
