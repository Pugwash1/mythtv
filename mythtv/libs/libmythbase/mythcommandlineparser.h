// -*- Mode: c++ -*-

#include <cstdint>   // for uint64_t
#include <utility>

#include <QStringList>
#include <QDateTime>
#include <QSize>
#include <QMap>
#include <QString>
#include <QVariant>

#include "mythbaseexp.h"
#include "mythlogging.h"
#include "referencecounter.h"

class MythCommandLineParser;

class MBASE_PUBLIC CommandLineArg : public ReferenceCounter
{
  public:
    CommandLineArg(const QString& name, QVariant::Type type, QVariant def,
                   QString help, QString longhelp);
    CommandLineArg(const QString& name, QVariant::Type type, QVariant def);
    explicit CommandLineArg(const QString& name);
   ~CommandLineArg() override = default;

    CommandLineArg* SetGroup(const QString &group)    { m_group = group;
                                                      return this; }
    void            AddKeyword(const QString &keyword) { m_keywords << keyword; }

    QString         GetName(void) const             { return m_name; }
    QString         GetUsedKeyword(void) const      { return m_usedKeyword; }
    int             GetKeywordLength(void) const;
    QString         GetHelpString(int off, const QString& group = "",
                                  bool force = false) const;
    QString         GetLongHelpString(QString keyword) const;

    bool            Set(const QString& opt);
    bool            Set(const QString& opt, const QByteArray& val);
    void            Set(QVariant val)               { m_stored = std::move(val);
                                                      m_given = true; }

    CommandLineArg* SetParent(const QString &opt);
    CommandLineArg* SetParent(const QStringList& opts);
    CommandLineArg* SetParentOf(const QString &opt);
    CommandLineArg* SetParentOf(const QStringList& opts);

    CommandLineArg* SetChild(const QString& opt);
    CommandLineArg* SetChild(const QStringList& opt);
    CommandLineArg* SetChildOf(const QString& opt);
    CommandLineArg* SetChildOf(const QStringList& opts);

    CommandLineArg* SetRequiredChild(const QString& opt);
    CommandLineArg* SetRequiredChild(const QStringList& opt);
    CommandLineArg* SetRequiredChildOf(const QString& opt);
    CommandLineArg* SetRequiredChildOf(const QStringList& opt);

    CommandLineArg* SetRequires(const QString &opt);
    CommandLineArg* SetRequires(const QStringList& opts);
    CommandLineArg* SetBlocks(const QString &opt);
    CommandLineArg* SetBlocks(const QStringList& opts);

    CommandLineArg* SetDeprecated(QString depstr = "");
    CommandLineArg* SetRemoved(QString remstr = "", QString remver = "");

    static void     AllowOneOf(const QList<CommandLineArg*>& args);

    void            PrintVerbose(void) const;

    friend class MythCommandLineParser;

  private:
    QString GetKeywordString(void) const;

    void            SetParentOf(CommandLineArg *other, bool forward = true);
    void            SetChildOf(CommandLineArg *other, bool forward = true);
    void            SetRequires(CommandLineArg *other, bool forward = true);
    void            SetBlocks(CommandLineArg *other, bool forward = true);

    void            Convert(void);

    QString         GetPreferredKeyword(void) const;
    bool            TestLinks(void) const;
    void            CleanupLinks(void);

    void            PrintRemovedWarning(QString &keyword) const;
    void            PrintDeprecatedWarning(QString &keyword) const;

    bool                    m_given     {false};
    bool                    m_converted {false};
    QString                 m_name;
    QString                 m_group;
    QString                 m_deprecated;
    QString                 m_removed;
    QString                 m_removedversion;
    QVariant::Type          m_type      {QVariant::Invalid};
    QVariant                m_default;
    QVariant                m_stored;

    QStringList             m_keywords;
    QString                 m_usedKeyword;

    QList<CommandLineArg*>  m_parents;
    QList<CommandLineArg*>  m_children;
    QList<CommandLineArg*>  m_requires;
    QList<CommandLineArg*>  m_requiredby;
    QList<CommandLineArg*>  m_blocks;

    QString                 m_help;
    QString                 m_longhelp;
};

class MBASE_PUBLIC MythCommandLineParser
{
  public:
    enum class Result {
        kEnd          = 0,
        kEmpty        = 1,
        kOptOnly      = 2,
        kOptVal       = 3,
        kCombOptVal   = 4,
        kArg          = 5,
        kPassthrough  = 6,
        kInvalid      = 7
    };

    explicit MythCommandLineParser(QString appname);
   ~MythCommandLineParser();

    virtual void LoadArguments(void) {};
    static void PrintVersion(void) ;
    void PrintHelp(void) const;
    QString GetHelpString(void) const;
    virtual QString GetHelpHeader(void) const { return ""; }

    static const char* NamedOptType(Result type);
    virtual bool Parse(int argc, const char * const * argv);

// overloaded add constructors for single string options
    // bool with default
    CommandLineArg* add(const QString& arg, const QString& name, bool def,
                        QString help, QString longhelp)
          { return add(QStringList(arg), name, QVariant::Bool,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // int
    CommandLineArg* add(const QString& arg, const QString& name, int def,
                        QString help, QString longhelp)
          { return add(QStringList(arg), name, QVariant::Int,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // uint
    CommandLineArg* add(const QString& arg, const QString& name, uint def,
             QString help, QString longhelp)
          { return add(QStringList(arg), name, QVariant::UInt,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // long long
    CommandLineArg* add(const QString& arg, const QString& name, long long def,
             QString help, QString longhelp)
          { return add(QStringList(arg), name, QVariant::LongLong,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // double
    CommandLineArg* add(const QString& arg, const QString& name, double def,
             QString help, QString longhelp)
          { return add(QStringList(arg), name, QVariant::Double,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // const char *
    CommandLineArg* add(const QString& arg, const QString& name, const char *def,
             QString help, QString longhelp)
          { return add(QStringList(arg), name, QVariant::String,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // QString
    CommandLineArg* add(const QString& arg, const QString& name, const QString& def,
             QString help, QString longhelp)
          { return add(QStringList(arg), name, QVariant::String,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // QSize
    CommandLineArg* add(const QString& arg, const QString& name, QSize def,
             QString help, QString longhelp)
          { return add(QStringList(arg), name, QVariant::Size,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // QDateTime
    CommandLineArg* add(const QString& arg, const QString& name, const QDateTime& def,
             QString help, QString longhelp)
          { return add(QStringList(arg), name, QVariant::DateTime,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // anything else
    CommandLineArg* add(const QString& arg, const QString& name, QVariant::Type type,
             QString help, QString longhelp)
          { return add(QStringList(arg), name, type,
                       QVariant(type), std::move(help), std::move(longhelp)); }
    // anything else with default
    CommandLineArg* add(const QString& arg, const QString& name, QVariant::Type type,
             QVariant def, QString help, QString longhelp)
          { return add(QStringList(arg), name, type,
                       std::move(def), std::move(help), std::move(longhelp)); }

// overloaded add constructors for multi-string options
    // bool with default
    CommandLineArg* add(QStringList arglist, const QString& name, bool def,
             QString help, QString longhelp)
          { return add(std::move(arglist), name, QVariant::Bool,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // int
    CommandLineArg* add(QStringList arglist, const QString& name, int def,
             QString help, QString longhelp)
          { return add(std::move(arglist), name, QVariant::Int,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // uint
    CommandLineArg* add(QStringList arglist, const QString& name, uint def,
             QString help, QString longhelp)
          { return add(std::move(arglist), name, QVariant::UInt,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // long long
    CommandLineArg* add(QStringList arglist, const QString& name, long long def,
             QString help, QString longhelp)
          { return add(std::move(arglist), name, QVariant::LongLong,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // float
    CommandLineArg* add(QStringList arglist, const QString& name, double def,
             QString help, QString longhelp)
          { return add(std::move(arglist), name, QVariant::Double,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // const char *
    CommandLineArg* add(QStringList arglist, const QString& name, const char *def,
             QString help, QString longhelp)
          { return add(std::move(arglist), name, QVariant::String,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // QString
    CommandLineArg* add(QStringList arglist, const QString& name, const QString& def,
             QString help, QString longhelp)
          { return add(std::move(arglist), name, QVariant::String,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // QSize
    CommandLineArg* add(QStringList arglist, const QString& name, QSize def,
             QString help, QString longhelp)
          { return add(std::move(arglist), name, QVariant::Size,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // QDateTime
    CommandLineArg* add(QStringList arglist, const QString& name, const QDateTime& def,
             QString help, QString longhelp)
          { return add(std::move(arglist), name, QVariant::DateTime,
                       QVariant(def), std::move(help), std::move(longhelp)); }
    // anything else
    CommandLineArg* add(QStringList arglist, const QString& name, QVariant::Type type,
             QString help, QString longhelp)
          { return add(std::move(arglist), name, type,
                       QVariant(type), std::move(help), std::move(longhelp)); }
    // anything else with default
    CommandLineArg* add(QStringList arglist, const QString& name, QVariant::Type type,
             QVariant def, QString help, QString longhelp);

    QVariant                operator[](const QString &name);
    QStringList             GetArgs(void) const;
    QMap<QString,QString>   GetExtra(void) const;
    QString                 GetPassthrough(void) const;
    QMap<QString,QString>   GetSettingsOverride(void);
    QString                 GetLogFilePath(void);
    int                     GetSyslogFacility(void) const;
    LogLevel_t              GetLogLevel(void) const;
    QString                 GetAppName(void) const { return m_appname; }

    bool                    toBool(const QString& key) const;
    int                     toInt(const QString& key) const;
    uint                    toUInt(const QString& key) const;
    long long               toLongLong(const QString& key) const;
    double                  toDouble(const QString& key) const;
    QSize                   toSize(const QString& key) const;
    QString                 toString(const QString& key) const;
    QStringList             toStringList(const QString& key, const QString& sep = "") const;
    QMap<QString,QString>   toMap(const QString& key) const;
    QDateTime               toDateTime(const QString& key) const;

    bool                    SetValue(const QString &key, const QVariant& value);
    int                     ConfigureLogging(const QString& mask = "general",
                                             bool progress = false);
    void                    ApplySettingsOverride(void);
    int                     Daemonize(void) const;

  protected:
    void allowArgs(bool allow=true);
    void allowExtras(bool allow=true);
    void allowPassthrough(bool allow=true);

    void addHelp(void);
    void addVersion(void);
    void addWindowed(void);
    void addMouse(void);
    void addDaemon(void);
    void addSettingsOverride(void);
    void addRecording(void);
    void addGeometry(void);
    void addDisplay(void);
    void addUPnP(void);
    void addLogging(const QString &defaultVerbosity = "general",
                    LogLevel_t defaultLogLevel = LOG_INFO);
    void addPIDFile(void);
    void addJob(void);
    void addInFile(bool addOutFile = false);

  private:
    Result getOpt(int argc, const char * const * argv, int &argpos,
               QString &opt, QByteArray &val);
    bool ReconcileLinks(void);

    QString                         m_appname;
    QMap<QString,CommandLineArg*>   m_optionedArgs;
    QMap<QString,CommandLineArg*>   m_namedArgs;
    bool                            m_passthroughActive {false};
    bool                            m_overridesImported {false};
    bool                            m_verbose           {false};
};

Q_DECLARE_METATYPE(MythCommandLineParser::Result)
