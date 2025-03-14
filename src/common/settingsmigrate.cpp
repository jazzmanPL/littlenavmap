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

#include "common/settingsmigrate.h"

#include "settings/settings.h"
#include "common/constants.h"
#include "util/version.h"
#include "options/optiondata.h"

#include <QDebug>
#include <QApplication>
#include <QRegularExpression>
#include <QFile>
#include <QDir>
#include <QSettings>
#include <QFont>

using atools::settings::Settings;
using  atools::util::Version;

namespace migrate {

static Version optionsVersion;

void removeAndLog(Settings& settings, const QString& key)
{
  if(settings.contains(key))
  {
    qInfo() << Q_FUNC_INFO << "Removing" << QFileInfo(settings.getFilename()).fileName() << key;
    settings.remove(key);
  }
}

void checkAndMigrateSettings()
{
  Settings& settings = Settings::instance();

  optionsVersion = Version(settings.valueStr(lnm::OPTIONS_VERSION));
  Version programVersion;

  if(optionsVersion.isValid())
  {
    qInfo() << Q_FUNC_INFO << "Options" << optionsVersion << "program" << programVersion;

    // Migrate map style file =======================================================================
    QFile nightstyleFile(Settings::getPath() + QDir::separator() + "little_navmap_nightstyle.ini");
    QFile mapstyleFile(Settings::getPath() + QDir::separator() + "little_navmap_mapstyle.ini");
    QSettings mapstyleSettings(mapstyleFile.fileName(), QSettings::IniFormat);
    QSettings nightstyleSettings(nightstyleFile.fileName(), QSettings::IniFormat);
    Version mapstyleVersion(mapstyleSettings.value("Options/Version").toString());

    // No version or old
    if(!mapstyleVersion.isValid() || mapstyleVersion < Version("2.0.1.beta"))
    {
      qInfo() << Q_FUNC_INFO << "Moving little_navmap_mapstyle.ini to backup";

      // Backup with version name
      QString newName = Settings::getPath() + QDir::separator() + "little_navmap_mapstyle_backup" +
                        (mapstyleVersion.isValid() ? "_" + mapstyleVersion.getVersionString() : "") +
                        ".ini";

      // Rename so LNM can create a new one later
      if(mapstyleFile.rename(newName))
        qInfo() << "Renamed" << mapstyleFile.fileName() << "to" << newName;
      else
        qWarning() << "Renaming" << mapstyleFile.fileName() << "to" << newName << "failed";
    }

    // Migrate settings =======================================================================
    if(optionsVersion != programVersion)
    {
      qInfo() << Q_FUNC_INFO << "Found settings version mismatch. Settings file version" <<
        optionsVersion << "Program version" << programVersion << ".";

      // ===============================================================
      if(optionsVersion <= Version("2.0.2"))
      {
        // CenterRadiusACC=60 and CenterRadiusFIR=60
        qInfo() << Q_FUNC_INFO << "Adjusting Online/CenterRadiusACC and Online/CenterRadiusFIR";
        if(settings.valueInt("Online/CenterRadiusACC", -1) == -1)
          settings.setValue("Online/CenterRadiusACC", 100);
        if(settings.valueInt("Online/CenterRadiusFIR", -1) == -1)
          settings.setValue("Online/CenterRadiusFIR", 100);
        settings.syncSettings();
      }

      // ===============================================================
      if(optionsVersion < Version("2.2.4"))
      {
        qInfo() << Q_FUNC_INFO << "Adjusting NOAA URL";
        // http://tgftp.nws.noaa.gov/data/observations/metar/stations/%1.TXT to
        // https://tgftp.nws.noaa.gov/data/observations/metar/stations/%1.TXT
        // in widget Widget_lineEditOptionsWeatherNoaaUrl

        // Widget_lineEditOptionsWeatherNoaaUrl=http://tgftp.nws.noaa.gov/data/observations/metar/stations/%1.TXT
        QString noaaUrl = settings.valueStr("OptionsDialog/Widget_lineEditOptionsWeatherNoaaUrl");

        if(noaaUrl.isEmpty() ||
           noaaUrl.toLower() == "http://tgftp.nws.noaa.gov/data/observations/metar/stations/%1.txt")
        {
          qInfo() << Q_FUNC_INFO << "Changing NOAA URL to HTTPS";
          // NOTE: Need to cast to QString to avoid using the overloaded boolean method
          settings.setValue("OptionsDialog/Widget_lineEditOptionsWeatherNoaaUrl",
                            QString("https://tgftp.nws.noaa.gov/data/observations/metar/stations/%1.TXT"));
          settings.syncSettings();
        }
      }

      // ===============================================================
      if(optionsVersion < Version("2.4.2.beta"))
      {
        qInfo() << Q_FUNC_INFO << "Adjusting settings for versions before 2.4.2.beta";
        removeAndLog(settings, lnm::ROUTE_STRING_DIALOG_OPTIONS);
        settings.syncSettings();

        nightstyleSettings.remove("StyleColors/Disabled_WindowText");
        nightstyleSettings.sync();
      }

      // ===============================================================
      if(optionsVersion < Version("2.4.3.rc1"))
      {
        qInfo() << Q_FUNC_INFO << "Adjusting settings for versions before 2.4.3.rc1";
        removeAndLog(settings, "MainWindow/Widget_mapThemeComboBox");
        settings.syncSettings();
      }

      // ===============================================================
      if(optionsVersion <= Version("2.4.5"))
      {
        qInfo() << Q_FUNC_INFO << "Adjusting settings for versions before or equal to 2.4.5";

        // Route view
        removeAndLog(settings, "Route/View_tableViewRoute");

        // Table columns dialog
        removeAndLog(settings, "Route/FlightPlanTableColumnsCheckBoxStates");

        // Route tabs
        removeAndLog(settings, "Route/WidgetTabsTabIds");
        removeAndLog(settings, "Route/WidgetTabsCurrentTabId");
        removeAndLog(settings, "Route/WidgetTabsLocked");

        // Reset all before flight
        removeAndLog(settings, "Route/ResetAllDialogCheckBoxStates");

        // Complete log search options
        qInfo() << Q_FUNC_INFO << "Removing" << QFileInfo(settings.getFilename()).fileName() << "SearchPaneLogdata";
        settings.remove("SearchPaneLogdata");

        // Search views
        removeAndLog(settings, "SearchPaneAirport/WidgetView_tableViewAirportSearch");
        removeAndLog(settings, "SearchPaneAirport/WidgetDistView_tableViewAirportSearch");
        removeAndLog(settings, "SearchPaneNav/WidgetView_tableViewAirportSearch");
        removeAndLog(settings, "SearchPaneNav/WidgetDistView_tableViewAirportSearch");
        removeAndLog(settings, "SearchPaneUserdata/WidgetView_tableViewUserdata");

        // info tabs
        removeAndLog(settings, "InfoWindow/WidgetTabsTabIds");
        removeAndLog(settings, "InfoWindow/WidgetTabsCurrentTabId");
        removeAndLog(settings, "InfoWindow/WidgetTabsLocked");

        // Choice dialog import and export
        removeAndLog(settings, "UserdataExport/ChoiceDialogCheckBoxStates");
        removeAndLog(settings, "Logdata/CsvExportCheckBoxStates");

        // Range rings
        removeAndLog(settings, lnm::MAP_RANGEMARKERS);

        // Marble plugins
        for(const QString& key : settings.childGroups())
          if(key.startsWith("plugin_"))
            removeAndLog(settings, key);

        // and vatsim URL,
        settings.setValue("OptionsDialog/Widget_lineEditOptionsWeatherVatsimUrl",
                          QString("https://metar.vatsim.net/metar.php?id=ALL"));

        // clear allow undock map?

        settings.syncSettings();
      }

      if(optionsVersion <= Version("2.6.0.beta"))
      {
        qInfo() << Q_FUNC_INFO << "Adjusting settings for versions before or equal to 2.6.0.beta";
        removeAndLog(settings, "SearchPaneLogdata/WidgetView_tableViewLogdata");
      }

      if(optionsVersion <= Version("2.6.1.beta"))
      {
        qInfo() << Q_FUNC_INFO << "Adjusting settings for versions before or equal to 2.6.1.beta";
        removeAndLog(settings, lnm::ROUTE_EXPORT_FORMATS);
        removeAndLog(settings, "RouteExport/RouteExportDialog_tableViewRouteExport");
        removeAndLog(settings, "RouteExport/RouteExportDialog_RouteMultiExportDialog_size");
        removeAndLog(settings, "RouteExport/RouteExportDialog_tableViewRouteExport");
      }

      if(optionsVersion <= Version("2.6.6"))
      {
        qInfo() << Q_FUNC_INFO << "Adjusting settings for versions before or equal to 2.6.6";
        settings.setValue("MainWindow/Widget_statusBar", true);
      }

      // =====================================================================
      // Adapt update channels if not yet saved or previous version is stable and this one is not
      if(!settings.contains(lnm::OPTIONS_UPDATE_CHANNELS) || (optionsVersion.isStable() && !programVersion.isStable()))
      {
        // No channel assigned yet or user moved from a stable to beta or development version
        if(programVersion.isBeta() || programVersion.isReleaseCandidate())
        {
          qInfo() << Q_FUNC_INFO << "Adjusting update channel to beta";
          settings.setValue(lnm::OPTIONS_UPDATE_CHANNELS, opts::STABLE_BETA);
        }
        else if(programVersion.isDevelop())
        {
          qInfo() << Q_FUNC_INFO << "Adjusting update channel to develop";
          settings.setValue(lnm::OPTIONS_UPDATE_CHANNELS, opts::STABLE_BETA_DEVELOP);
        }
      }

      if(optionsVersion <= Version("2.6.13"))
      {
        qInfo() << Q_FUNC_INFO << "Adjusting settings for versions before or equal to 2.6.13";
        removeAndLog(settings, "SearchPaneOnlineCenter/WidgetView_tableViewOnlineCenterSearch");
        removeAndLog(settings, "SearchPaneOnlineClient/WidgetView_tableViewOnlineClientSearch");
        removeAndLog(settings, "SearchPaneOnlineServer/WidgetView_tableViewOnlineServerSearch");
      }

      if(optionsVersion <= Version("2.6.14"))
      {
        qInfo() << Q_FUNC_INFO << "Adjusting settings for versions before or equal to 2.6.14";
        removeAndLog(settings, "SearchPaneAirport/WidgetDistView_tableViewAirportSearch");
        removeAndLog(settings, "SearchPaneAirport/WidgetView_tableViewAirportSearch");
        removeAndLog(settings, "OptionsDialog/Widget_lineEditOptionsWeatherVatsimUrl");
      }

      if(optionsVersion <= Version("2.6.16"))
      {
        qInfo() << Q_FUNC_INFO << "Adjusting settings for versions before or equal to 2.6.16";
        removeAndLog(settings, "OptionsDialog/DisplayOptionsuserAircraft_2097152"); // ITEM_USER_AIRCRAFT_COORDINATES
        removeAndLog(settings, "OptionsDialog/DisplayOptionsAiAircraft_2"); // ITEM_AI_AIRCRAFT_COORDINATES
        removeAndLog(settings, "Route/View_tableViewRoute");
        removeAndLog(settings, "OptionsDialog/Widget_lineEditOptionsWeatherIvaoUrl");
        removeAndLog(settings, "Map/DetailFactor");
      }

      // Set program version to options and save ===================
      settings.setValue(lnm::OPTIONS_VERSION, programVersion.getVersionString());
      settings.syncSettings();
    }
  }
  else
  {
    qWarning() << Q_FUNC_INFO << "No version information found in settings file. Updating to" << programVersion;
    settings.setValue(lnm::OPTIONS_VERSION, programVersion.getVersionString());
    settings.syncSettings();
  }

  // Always correct map font if missing
  if(!settings.contains(lnm::OPTIONS_DIALOG_MAP_FONT))
  {
    qInfo() << Q_FUNC_INFO << "Adjusting map font";
    // Make map font a bold copy of system font if no setting present
    QFont font(QGuiApplication::font());
    font.setBold(true);
    settings.setValueVar(lnm::OPTIONS_DIALOG_MAP_FONT, font);
    settings.syncSettings();
  }
}

const Version& getOptionsVersion()
{
  return optionsVersion;
}

} // namespace migrate
