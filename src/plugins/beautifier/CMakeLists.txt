add_qtc_plugin(Beautifier
  DEPENDS Qt::Xml
  PLUGIN_DEPENDS Core ProjectExplorer TextEditor
  SOURCES
    abstractsettings.cpp abstractsettings.h
    artisticstyle/artisticstyle.cpp artisticstyle/artisticstyle.h
    artisticstyle/artisticstyleconstants.h
    artisticstyle/artisticstyleoptionspage.cpp artisticstyle/artisticstyleoptionspage.h
    artisticstyle/artisticstylesettings.cpp artisticstyle/artisticstylesettings.h
    beautifier.qrc
    beautifierabstracttool.h
    beautifierconstants.h
    beautifierplugin.cpp beautifierplugin.h
    clangformat/clangformat.cpp clangformat/clangformat.h
    clangformat/clangformatconstants.h
    clangformat/clangformatoptionspage.cpp clangformat/clangformatoptionspage.h
    clangformat/clangformatsettings.cpp clangformat/clangformatsettings.h
    configurationdialog.cpp configurationdialog.h
    configurationeditor.cpp configurationeditor.h
    configurationpanel.cpp configurationpanel.h
    generaloptionspage.cpp generaloptionspage.h
    generalsettings.cpp generalsettings.h
    uncrustify/uncrustify.cpp uncrustify/uncrustify.h
    uncrustify/uncrustifyconstants.h
    uncrustify/uncrustifyoptionspage.cpp uncrustify/uncrustifyoptionspage.h
    uncrustify/uncrustifysettings.cpp uncrustify/uncrustifysettings.h
)
