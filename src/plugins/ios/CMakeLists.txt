add_qtc_plugin(Ios
  DEPENDS QmlDebug Qt::Xml
  PLUGIN_DEPENDS Core Debugger ProjectExplorer QmakeProjectManager CMakeProjectManager
  SOURCES
    createsimulatordialog.cpp createsimulatordialog.h
    ios.qrc
    iosbuildconfiguration.cpp iosbuildconfiguration.h
    iosbuildstep.cpp iosbuildstep.h
    iosconfigurations.cpp iosconfigurations.h
    iosconstants.h
    iosdeploystep.cpp iosdeploystep.h
    iosdevice.cpp iosdevice.h
    iosdsymbuildstep.cpp iosdsymbuildstep.h
    iosplugin.cpp iosplugin.h
    iosprobe.cpp iosprobe.h
    iosqtversion.cpp iosqtversion.h
    iosrunconfiguration.cpp iosrunconfiguration.h
    iosrunner.cpp iosrunner.h
    iossettingspage.cpp iossettingspage.h
    iossettingswidget.cpp iossettingswidget.h
    iossimulator.cpp iossimulator.h
    iostoolhandler.cpp iostoolhandler.h
    simulatorcontrol.cpp simulatorcontrol.h
    simulatorinfomodel.cpp simulatorinfomodel.h
    simulatoroperationdialog.cpp simulatoroperationdialog.h
)

extend_qtc_plugin(Ios
  CONDITION APPLE
  DEPENDS ${FWCoreFoundation} ${FWIOKit}
)
