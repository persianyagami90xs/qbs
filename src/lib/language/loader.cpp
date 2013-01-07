/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the Qt Build Suite.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "loader.h"

#include "artifactproperties.h"
#include "evaluationobject.h"
#include "identifiersearch.h"
#include "language.h"
#include "languageobject.h"
#include "probescope.h"
#include "projectfile.h"
#include "scopechain.h"
#include "scriptengine.h"

#include <jsextensions/file.h>
#include <jsextensions/process.h>
#include <jsextensions/textfile.h>
#include <logging/logger.h>
#include <logging/translator.h>
#include <parser/qmljsengine_p.h>
#include <parser/qmljslexer_p.h>
#include <parser/qmljsparser_p.h>
#include <tools/codelocation.h>
#include <tools/error.h>
#include <tools/fileinfo.h>
#include <tools/hostosinfo.h>
#include <tools/progressobserver.h>
#include <tools/scripttools.h>
#include <tools/settings.h>

#include <QDirIterator>

QT_BEGIN_NAMESPACE
static uint qHash(const QStringList &list)
{
    uint hash = 0;
    foreach (const QString &n, list)
        hash ^= qHash(n);
    return hash;
}
QT_END_NAMESPACE

const char QBS_LANGUAGE_VERSION[] = "1.0";

using namespace QbsQmlJS::AST;

namespace qbs {
namespace Internal {

class Loader::LoaderPrivate
{
public:
    LoaderPrivate(ScriptEngine *engine);

    void loadProject(const QString &fileName);
    ResolvedProjectPtr resolveProject(const QString &buildRoot,
                                        const QVariantMap &userProperties);
    ProjectFile::Ptr parseFile(const QString &fileName);

    void checkCancelation() const;
    void clearScopesCache();
    Scope::Ptr buildFileContext(ProjectFile *file);
    bool existsModuleInSearchPath(const QString &moduleName);
    Module::Ptr searchAndLoadModule(const QStringList &moduleId, const QString &moduleName, ScopeChain::Ptr moduleScope,
                                    const QVariantMap &userProperties, const CodeLocation &dependsLocation,
                                    const QStringList &extraSearchPaths = QStringList());
    Module::Ptr loadModule(ProjectFile *file, const QStringList &moduleId, const QString &moduleName, ScopeChain::Ptr moduleBaseScope,
                           const QVariantMap &userProperties, const CodeLocation &dependsLocation);
    void insertModulePropertyIntoScope(Scope::Ptr targetScope, const Module::Ptr &module, Scope::Ptr moduleInstance = Scope::Ptr());
    QList<Module::Ptr> evaluateDependency(LanguageObject *depends, ScopeChain::Ptr conditionScopeChain,
                                          ScopeChain::Ptr moduleScope, const QStringList &extraSearchPaths,
                                          QList<UnknownModule::Ptr> *unknownModules, const QVariantMap &userProperties);
    void evaluateDependencies(LanguageObject *object, EvaluationObject *evaluationObject, const ScopeChain::Ptr &localScope,
                              ScopeChain::Ptr moduleScope, const QVariantMap &userProperties, bool loadBaseModule = true);
    void evaluateDependencyConditions(EvaluationObject *evaluationObject);
    void evaluateImports(Scope::Ptr target, const JsImports &jsImports);
    void evaluatePropertyOptions(LanguageObject *object);
    QVariantMap evaluateAll(const ResolvedProductConstPtr &rproduct,
                            const Scope::Ptr &properties);
    QVariantMap evaluateModuleValues(const ResolvedProductConstPtr &rproduct,
                                     EvaluationObject *moduleContainer, Scope::Ptr objectScope);
    void resolveInheritance(LanguageObject *object, EvaluationObject *evaluationObject,
                            ScopeChain::Ptr moduleScope = ScopeChain::Ptr(), const QVariantMap &userProperties = QVariantMap());
    void fillEvaluationObject(const ScopeChain::Ptr &scope, LanguageObject *object, Scope::Ptr ids, EvaluationObject *evaluationObject, const QVariantMap &userProperties);
    void fillEvaluationObjectBasics(const ScopeChain::Ptr &scope, LanguageObject *object, EvaluationObject *evaluationObject);
    void setupBuiltinDeclarations();
    void setupInternalPrototype(LanguageObject *object, EvaluationObject *evaluationObject);
    void resolveModule(ResolvedProductPtr rproduct, const QString &moduleName, EvaluationObject *module);
    void resolveGroup(ResolvedProductPtr rproduct, EvaluationObject *product, EvaluationObject *group);
    void resolveProductModule(const ResolvedProductConstPtr &rproduct, EvaluationObject *group);
    void resolveTransformer(ResolvedProductPtr rproduct, EvaluationObject *trafo, ResolvedModuleConstPtr module);
    void resolveProbe(EvaluationObject *node);
    QList<EvaluationObject *> resolveCommonItems(const QList<EvaluationObject *> &objects,
                                                    ResolvedProductPtr rproduct, const ResolvedModuleConstPtr &module);
    RulePtr resolveRule(EvaluationObject *object, ResolvedModuleConstPtr module);
    FileTagger::ConstPtr resolveFileTagger(EvaluationObject *evaluationObject);
    void buildModulesProperty(EvaluationObject *evaluationObject);
    void checkModuleDependencies(const Module::Ptr &module);

    class ProductData
    {
    public:
        QString originalProductName;
        EvaluationObject *product;
        QList<UnknownModule::Ptr> usedProducts;
        QList<UnknownModule::Ptr> usedProductsFromProductModule;

        void addUsedProducts(const QList<UnknownModule::Ptr> &additionalUsedProducts, bool *productsAdded);
    };

    struct ProjectData
    {
        QHash<ResolvedProductPtr, ProductData> products;
    };

    void resolveTopLevel(const ResolvedProjectPtr &rproject,
                         LanguageObject *object,
                         const QString &projectFileName,
                         ProjectData *projectData,
                         QList<RulePtr> *globalRules,
                         QList<FileTagger::ConstPtr> *globalFileTaggers,
                         const QVariantMap &userProperties,
                         const ScopeChain::Ptr &scope,
                         const ResolvedModuleConstPtr &dummyModule);

    void checkUserProperties(const ProjectData &projectData, const QVariantMap &userProperties);
    typedef QMultiMap<QString, ResolvedProductPtr> ProductMap;
    void resolveProduct(const ResolvedProductPtr &product, const ResolvedProjectPtr &project,
                        ProductData &data, ProductMap &products,
                        const QList<RulePtr> &globalRules,
                        const QList<FileTagger::ConstPtr> &globalFileTaggers,
                        const ResolvedModuleConstPtr &dummyModule);
    void resolveProductDependencies(const ResolvedProjectPtr &project, ProjectData &projectData,
                                    const ProductMap &resolvedProducts);

    static LoaderPrivate *get(QScriptEngine *engine);
    static QScriptValue js_getHostOS(QScriptContext *context, QScriptEngine *engine);
    static QScriptValue js_getHostDefaultArchitecture(QScriptContext *context, QScriptEngine *engine);
    static QScriptValue js_getenv(QScriptContext *context, QScriptEngine *engine);
    static QScriptValue js_configurationValue(QScriptContext *context, QScriptEngine *engine);

    ProgressObserver *m_progressObserver;
    QStringList m_searchPaths;
    QStringList m_moduleSearchPaths;
    ScriptEngine * const m_engine;
    ScopesCachePtr m_scopesWithEvaluatedProperties;
    QScriptValue m_jsFunction_getHostOS;
    QScriptValue m_jsFunction_getHostDefaultArchitecture;
    QScriptValue m_jsFunction_getenv;
    QScriptValue m_jsFunction_configurationValue;
    QScriptValue m_probeScriptScope;
    Settings m_settings;
    ProjectFile::Ptr m_project;
    QHash<QString, ProjectFile::Ptr> m_parsedFiles;
    QHash<QString, PropertyDeclaration> m_dependsPropertyDeclarations;
    QHash<QString, PropertyDeclaration> m_groupPropertyDeclarations;
    QHash<QString, QList<PropertyDeclaration> > m_builtinDeclarations;
    QHash<QString, QVariantMap> m_productModules;
    QHash<QString, QStringList> m_moduleDirListCache;
    QHash<Scope::ConstPtr, QVariantMap> m_convertedScopesCache;
    QHash<QString, ProjectFile::Ptr> m_projectFiles;
};

const QString moduleSearchSubDir = QLatin1String("modules");

static QScriptValue evaluate(QScriptEngine *engine, const QScriptProgram &expression)
{
    QScriptValue result = engine->evaluate(expression);
    if (engine->hasUncaughtException()) {
        QString errorMessage = engine->uncaughtException().toString();
        int errorLine = engine->uncaughtExceptionLineNumber();
        engine->clearExceptions();
        throw Error(errorMessage, CodeLocation(expression.fileName(), errorLine));
    }
    if (result.isError())
        throw Error(result.toString());
    return result;
}

static QStringList resolvePaths(const QStringList &paths, const QString &base)
{
    QStringList resolved;
    foreach (const QString &path, paths) {
        QString resolvedPath = FileInfo::resolvePath(base, path);
        resolvedPath = QDir::cleanPath(resolvedPath);
        resolved += resolvedPath;
    }
    return resolved;
}


static const char szLoaderPropertyName[] = "qbs_loader_ptr";
static const QLatin1String name_FileTagger("FileTagger");
static const QLatin1String name_Rule("Rule");
static const QLatin1String name_Transformer("Transformer");
static const QLatin1String name_Artifact("Artifact");
static const QLatin1String name_Group("Group");
static const QLatin1String name_Project("Project");
static const QLatin1String name_Product("Product");
static const QLatin1String name_ProductModule("ProductModule");
static const QLatin1String name_Module("Module");
static const QLatin1String name_Properties("Properties");
static const QLatin1String name_PropertyOptions("PropertyOptions");
static const QLatin1String name_Depends("Depends");
static const QLatin1String name_moduleSearchPaths("moduleSearchPaths");
static const QLatin1String name_Probe("Probe");
static const uint hashName_FileTagger = qHash(name_FileTagger);
static const uint hashName_Rule = qHash(name_Rule);
static const uint hashName_Transformer = qHash(name_Transformer);
static const uint hashName_Artifact = qHash(name_Artifact);
static const uint hashName_Group = qHash(name_Group);
static const uint hashName_Project = qHash(name_Project);
static const uint hashName_Product = qHash(name_Product);
static const uint hashName_ProductModule = qHash(name_ProductModule);
static const uint hashName_Module = qHash(name_Module);
static const uint hashName_Properties = qHash(name_Properties);
static const uint hashName_PropertyOptions = qHash(name_PropertyOptions);
static const uint hashName_Depends = qHash(name_Depends);
static const uint hashName_Probe = qHash(name_Probe);
static const QLatin1String name_productPropertyScope("product property scope");
static const QLatin1String name_projectPropertyScope("project property scope");

Loader::Loader(ScriptEngine *engine) : d(new LoaderPrivate(engine))
{
}

Loader::~Loader()
{
    delete d;
}

void Loader::setProgressObserver(ProgressObserver *observer)
{
    d->m_progressObserver = observer;
}

static bool compare(const QStringList &list, const QString &value)
{
    if (list.size() != 1)
        return false;
    return list.first() == value;
}

void Loader::setSearchPaths(const QStringList &searchPaths)
{
    d->m_searchPaths.clear();
    foreach (const QString &searchPath, searchPaths) {
        if (!FileInfo::exists(searchPath)) {
            qbsWarning() << Tr::tr("Search path '%1' does not exist.")
                    .arg(QDir::toNativeSeparators(searchPath));
        } else {
            d->m_searchPaths << searchPath;
        }
    }

    d->m_moduleSearchPaths.clear();
    foreach (const QString &path, d->m_searchPaths)
        d->m_moduleSearchPaths += FileInfo::resolvePath(path, moduleSearchSubDir);
}

ResolvedProjectPtr Loader::loadProject(const QString &fileName, const QString &buildRoot,
                                         const QVariantMap &userProperties)
{
    TimedActivityLogger loadLogger(QLatin1String("Loading project"));
    d->loadProject(fileName);
    return d->resolveProject(buildRoot, userProperties);
}

QByteArray Loader::qmlTypeInfo()
{
    // Header:
    QByteArray result;
    result.append("import QtQuick.tooling 1.0\n\n");
    result.append("// This file describes the plugin-supplied types contained in the library.\n");
    result.append("// It is used for QML tooling purposes only.\n\n");
    result.append("Module {\n");

    // Individual Components:
    foreach (const QString &component, d->m_builtinDeclarations.keys()) {
        QByteArray componentName = component.toUtf8();
        result.append("    Component {\n");
        result.append(QByteArray("        name: \"") + componentName + QByteArray("\"\n"));
        result.append("        exports: [ \"qbs/");
        result.append(componentName);
        result.append(" ");
        result.append(QBS_LANGUAGE_VERSION);
        result.append("\" ]\n");
        result.append("        prototype: \"QQuickItem\"\n");

        QList<PropertyDeclaration> propertyList = d->m_builtinDeclarations.value(component);
        foreach (const PropertyDeclaration &property, propertyList) {
            result.append("        Property { name=\"");
            result.append(property.name.toUtf8());
            result.append("\"; ");
            switch (property.type) {
            case qbs::Internal::PropertyDeclaration::UnknownType:
                result.append("type=\"unknown\"");
                break;
            case qbs::Internal::PropertyDeclaration::Boolean:
                result.append("type=\"bool\"");
                break;
            case qbs::Internal::PropertyDeclaration::Path:
                result.append("type=\"string\"");
                break;
            case qbs::Internal::PropertyDeclaration::PathList:
                result.append("type=\"string\"; isList=true");
                break;
            case qbs::Internal::PropertyDeclaration::String:
                result.append("type=\"string\"");
                break;
            case qbs::Internal::PropertyDeclaration::Variant:
                result.append("type=\"QVariant\"");
                break;
            case qbs::Internal::PropertyDeclaration::Verbatim:
                result.append("type=\"string\"");
                break;
            }
            result.append(" }\n"); // Property
        }

        result.append("    }\n"); // Component
    }

    // Footer:
    result.append("}\n"); // Module
    return result;
}

Loader::LoaderPrivate::LoaderPrivate(ScriptEngine *engine)
    : m_progressObserver(0), m_engine(engine), m_scopesWithEvaluatedProperties(new ScopesCache)
{
    QVariant v;
    v.setValue(static_cast<void*>(this));
    engine->setProperty(szLoaderPropertyName, v);

    m_jsFunction_getHostOS  = engine->newFunction(js_getHostOS, 0);
    m_jsFunction_getHostDefaultArchitecture
            = engine->newFunction(js_getHostDefaultArchitecture, 0);
    m_jsFunction_getenv = engine->newFunction(js_getenv, 0);
    m_jsFunction_configurationValue = engine->newFunction(js_configurationValue, 2);
    setupBuiltinDeclarations();
}

void Loader::LoaderPrivate::loadProject(const QString &fileName)
{
    m_moduleDirListCache.clear();
    m_project = parseFile(fileName);
}

static void setPathAndFilePath(const Scope::Ptr &scope, const QString &filePath, const QString &prefix = QString())
{
    QString filePathPropertyName("filePath");
    QString pathPropertyName("path");
    if (!prefix.isEmpty()) {
        filePathPropertyName = prefix + QLatin1String("FilePath");
        pathPropertyName = prefix + QLatin1String("Path");
    }
    scope->properties.insert(filePathPropertyName, Property(QScriptValue(filePath)));
    scope->properties.insert(pathPropertyName, Property(QScriptValue(FileInfo::path(filePath))));
}

void Loader::LoaderPrivate::clearScopesCache()
{
    QScriptValue nullScriptValue;
    ScopesCache::const_iterator it = m_scopesWithEvaluatedProperties->begin();
    const ScopesCache::const_iterator scopeEnd = m_scopesWithEvaluatedProperties->end();
    for (; it != scopeEnd; ++it) {
        const QHash<QString, Property>::iterator propertiesEnd = (*it)->properties.end();
        for (QHash<QString, Property>::iterator pit = (*it)->properties.begin(); pit != propertiesEnd; ++pit) {
            Property &property = pit.value();
            if (!property.valueSource.isNull())
                property.value = nullScriptValue;
        }
    }
    m_scopesWithEvaluatedProperties->clear();
}

Scope::Ptr Loader::LoaderPrivate::buildFileContext(ProjectFile *file)
{
    Scope::Ptr context = Scope::create(m_engine, m_scopesWithEvaluatedProperties,
                                       QLatin1String("global file context"), file);
    setPathAndFilePath(context, file->fileName, QLatin1String("local"));
    evaluateImports(context, file->jsImports);

    return context;
}

bool Loader::LoaderPrivate::existsModuleInSearchPath(const QString &moduleName)
{
    foreach (const QString &dirPath, m_moduleSearchPaths)
        if (FileInfo(dirPath + QLatin1Char('/') + moduleName).exists())
            return true;
    return false;
}

void Loader::LoaderPrivate::resolveInheritance(LanguageObject *object, EvaluationObject *evaluationObject,
                                ScopeChain::Ptr moduleScope, const QVariantMap &userProperties)
{
    if (object->prototypeFileName.isEmpty()) {
        if (object->prototype.size() != 1)
            throw Error(Tr::tr("prototype with dots does not resolve to a file"), object->prototypeLocation);
        evaluationObject->prototype = object->prototype.first();

        setupInternalPrototype(object, evaluationObject);

        // once we know something is a project/product, add a property to
        // the correct scope
        if (moduleScope) {
            if (evaluationObject->prototype == name_Project) {
                if (Scope::Ptr projectPropertyScope = moduleScope->find(name_projectPropertyScope))
                    projectPropertyScope->properties.insert("project", Property(evaluationObject));
            }
            else if (evaluationObject->prototype == name_Product) {
                if (Scope::Ptr productPropertyScope = moduleScope->find(name_productPropertyScope))
                    productPropertyScope->properties.insert("product", Property(evaluationObject));
            }
        }

        return;
    }

    // load prototype (cache result)
    ProjectFile::Ptr file = parseFile(object->prototypeFileName);

    // recurse to prototype's prototype
    if (evaluationObject->objects.contains(file->root)) {
        QString msg = Tr::tr("circular prototypes in instantiation of '%1', '%2' recurred");
        throw Error(msg.arg(evaluationObject->instantiatingObject()->prototype.join("."),
                                   object->prototype.join(".")));
    }
    evaluationObject->objects.append(file->root);
    resolveInheritance(file->root, evaluationObject, moduleScope, userProperties);

    // ### expensive, and could be shared among all builds of this prototype instance
    Scope::Ptr context = buildFileContext(file.data());

    // project and product scopes are always available
    ScopeChain::Ptr scopeChain(new ScopeChain(m_engine, context));
    if (moduleScope) {
        if (Scope::Ptr projectPropertyScope = moduleScope->findNonEmpty(name_projectPropertyScope))
            scopeChain->prepend(projectPropertyScope);
        if (Scope::Ptr productPropertyScope = moduleScope->findNonEmpty(name_productPropertyScope))
            scopeChain->prepend(productPropertyScope);
    }

    scopeChain->prepend(evaluationObject->scope);

    // having a module scope enables resolving of Depends blocks
    if (moduleScope)
        evaluateDependencies(file->root, evaluationObject, scopeChain, moduleScope, userProperties);

    fillEvaluationObject(scopeChain, file->root, evaluationObject->scope, evaluationObject, userProperties);

//    QByteArray indent;
//    evaluationObject->dump(indent);
}

static bool checkFileCondition(QScriptEngine *engine, const ScopeChain::Ptr &scope, const ProjectFile *file)
{
    static const bool debugCondition = false;
    if (debugCondition)
        qbsTrace() << "Checking condition";

    const Binding &condition = file->root->bindings.value(QStringList("condition"));
    if (!condition.isValid())
        return true;

    QScriptContext *context = engine->currentContext();
    const QScriptValue oldActivation = context->activationObject();
    context->setActivationObject(scope->value());

    if (debugCondition)
        qbsTrace() << "   code is: " << condition.valueSource.sourceCode();
    const QScriptValue value = evaluate(engine, condition.valueSource);
    bool result = false;
    if (value.isBool())
        result = value.toBool();
    else
        throw Error(Tr::tr("Condition return type must be boolean."), condition.codeLocation());
    if (debugCondition)
        qbsTrace() << "   result: " << value.toString();

    context->setActivationObject(oldActivation);
    return result;
}

static void applyFunctions(QScriptEngine *engine, LanguageObject *object, EvaluationObject *evaluationObject)
{
    foreach (const Function &func, object->functions) {
        Property property;
        property.value = evaluate(engine, func.source);
        evaluationObject->scope->properties.insert(func.name, property);
    }
}

static void applyFunctions(QScriptEngine *engine, LanguageObject *object, EvaluationObject *evaluationObject, const QScriptValue &value)
{
    if (object->functions.isEmpty())
        return;

    // set the activation object to the correct scope
    QScriptValue oldActivation = engine->currentContext()->activationObject();
    engine->currentContext()->setActivationObject(value);

    applyFunctions(engine, object, evaluationObject);

    engine->currentContext()->setActivationObject(oldActivation);
}

static void applyFunctions(QScriptEngine *engine, LanguageObject *object, EvaluationObject *evaluationObject, ScopeChain::Ptr scope)
{
    return applyFunctions(engine, object, evaluationObject, scope->value());
}

static void applyBinding(LanguageObject *object, const Binding &binding, const ScopeChain::Ptr &scopeChain)
{
    Scope *target;
    if (binding.name.size() == 1) {
        target = scopeChain->first().data(); // assume the top scope is the 'current' one
    } else {
        if (compare(object->prototype, name_Artifact))
            return;
        QScriptValue targetValue = scopeChain->value().property(binding.name.first());
        if (!targetValue.isValid() || targetValue.isError()) {
            QString msg = Tr::tr("Binding '%1' failed, no property '%2' in the scope of %3");
            throw Error(msg.arg(binding.name.join("."),
                                       binding.name.first(),
                                       scopeChain->first()->name()),
                               binding.codeLocation());
        }
        target = dynamic_cast<Scope *>(targetValue.scriptClass());
        if (!target) {
            QString msg = Tr::tr("Binding '%1' failed, property '%2' in the scope of %3 has no properties");
            throw Error(msg.arg(binding.name.join("."),
                                       binding.name.first(),
                                       scopeChain->first()->name()),
                               binding.codeLocation());
        }
    }

    for (int i = 1; i < binding.name.size() - 1; ++i) {
        Scope *oldTarget = target;
        const QString &bindingName = binding.name.at(i);
        const QScriptValue &value = target->property(bindingName);
        if (!value.isValid()) {
            QString msg = Tr::tr("Binding '%1' failed, no property '%2' in %3");
            throw Error(msg.arg(binding.name.join("."),
                                       binding.name.at(i),
                                       target->name()),
                               binding.codeLocation());
        }
        target = dynamic_cast<Scope *>(value.scriptClass());
        if (!target) {
            QString msg = Tr::tr("Binding '%1' failed, property '%2' in %3 has no properties");
            throw Error(msg.arg(binding.name.join("."),
                                       bindingName,
                                       oldTarget->name()),
                               binding.codeLocation());
        }
    }

    const QString name = binding.name.last();

    if (!target->declarations.contains(name)) {
        QString msg = Tr::tr("Binding '%1' failed, no property '%2' in %3");
        throw Error(msg.arg(binding.name.join("."),
                                   name,
                                   target->name()),
                           binding.codeLocation());
    }

    Property newProperty;
    newProperty.valueSource = binding.valueSource;
    newProperty.valueSourceUsesBase = binding.valueSourceUsesBase;
    newProperty.valueSourceUsesOuter = binding.valueSourceUsesOuter;
    newProperty.scopeChain = scopeChain;
    newProperty.sourceObject = object;

    Property &property = target->properties[name];
    if (!property.valueSource.isNull()) {
        newProperty.baseProperties += property.baseProperties;
        property.baseProperties.clear();
        newProperty.baseProperties += property;
    }
    property = newProperty;
}

static void applyBindings(LanguageObject *object, const ScopeChain::Ptr &scopeChain)
{
    foreach (const Binding &binding, object->bindings)
        applyBinding(object, binding, scopeChain);
}

void Loader::LoaderPrivate::setupInternalPrototype(LanguageObject *object, EvaluationObject *evaluationObject)
{
    if (!m_builtinDeclarations.contains(evaluationObject->prototype))
        throw Error(Tr::tr("Type name '%1' is unknown.").arg(evaluationObject->prototype),
                           evaluationObject->instantiatingObject()->prototypeLocation);

    foreach (const PropertyDeclaration &pd, m_builtinDeclarations.value(evaluationObject->prototype)) {
        evaluationObject->scope->declarations.insert(pd.name, pd);
        evaluationObject->scope->properties.insert(pd.name, Property(m_engine->undefinedValue()));

        // If there's an initial value and the language object doesn't have a binding for that property, create one.
        if (!pd.initialValueSource.isEmpty()) {
            const QStringList bindingName(pd.name);
            Binding &binding = object->bindings[bindingName];
            if (!binding.isValid()) {
                binding.name = bindingName;
                binding.valueSource = QScriptProgram(pd.initialValueSource);
            }
        }
    }
}

struct ConditionalBinding
{
    QString condition;
    QString rhs;
};

void Loader::LoaderPrivate::fillEvaluationObject(const ScopeChain::Ptr &scope, LanguageObject *object, Scope::Ptr ids, EvaluationObject *evaluationObject, const QVariantMap &userProperties)
{
    // filter Properties items
    typedef QHash<QStringList, QVector<ConditionalBinding> > ConditionalBindings;
    ConditionalBindings conditionalBindings;
    QList<LanguageObject *>::iterator childIt = object->children.begin();
    while (childIt != object->children.end()) {
        LanguageObject *propertiesItem = *childIt;
        if (!compare(propertiesItem->prototype, name_Properties)) {
            ++childIt;
            continue;
        }
        childIt = object->children.erase(childIt);

        if (!propertiesItem->children.isEmpty())
            throw Error(Tr::tr("Properties block may not have children"), propertiesItem->children.first()->prototypeLocation);

        const QStringList conditionName("condition");
        const Binding condition = propertiesItem->bindings.value(conditionName);
        if (!condition.isValid())
            throw Error(Tr::tr("Properties block must have a condition property"), propertiesItem->prototypeLocation);

        const QString conditionCode = condition.valueSource.sourceCode();
        QHashIterator<QStringList, Binding> it(propertiesItem->bindings);
        while (it.hasNext()) {
            it.next();
            if (it.key() == conditionName)
                continue;

            const Binding &binding = it.value();
            ConditionalBinding conditionalBinding;
            conditionalBinding.condition = conditionCode;
            conditionalBinding.rhs = binding.valueSource.sourceCode();
            conditionalBindings[binding.name] += conditionalBinding;
        }
    }

    // rewrite conditional bindings
    for (ConditionalBindings::const_iterator it = conditionalBindings.constBegin(); it != conditionalBindings.constEnd(); ++it) {
        const QStringList &bindingName = it.key();
        Binding &parentBinding = object->bindings[bindingName];
        QString rewrittenSource = QLatin1String("(function() {\n"
                                                "var outer = ");
        if (parentBinding.isValid()) {
            rewrittenSource += parentBinding.valueSource.sourceCode() + QLatin1Char('\n');
        } else {
            rewrittenSource += QLatin1String("undefined\n");
            parentBinding.name = bindingName;
        }

        foreach (const ConditionalBinding &cb, it.value())
            rewrittenSource += QLatin1String("if (") + cb.condition
                    + QLatin1String(") outer = ") + cb.rhs + QLatin1Char('\n');

        rewrittenSource += QLatin1String("return outer})()");
        parentBinding.valueSource = rewrittenSource;
    }

    // fill subobjects recursively
    foreach (LanguageObject *child, object->children) {
        // 'Depends' blocks are already handled before this function is called
        // and should not appear in the children list
        if (compare(child->prototype, name_Depends))
            continue;

        EvaluationObject *childEvObject = new EvaluationObject(child);
        const QString propertiesName = child->prototype.join(QLatin1String("."));
        childEvObject->scope = Scope::create(m_engine, m_scopesWithEvaluatedProperties,
                                             propertiesName, object->file);

        resolveInheritance(child, childEvObject); // ### need to pass 'moduleScope' for product/project property scopes
        const uint childPrototypeHash = qHash(childEvObject->prototype);

        ScopeChain::Ptr childScope(scope->clone()->prepend(childEvObject->scope));

        if (!child->id.isEmpty()) {
            ids->properties.insert(child->id, Property(childEvObject));
        }

        const bool isProductModule = (childPrototypeHash == hashName_ProductModule);
        const bool isArtifact = (childPrototypeHash == hashName_Artifact);
        if (isProductModule) {
            // give ProductModules their own module namespace
            childScope = ScopeChain::Ptr(new ScopeChain(m_engine, childEvObject->scope));
            ScopeChain::Ptr moduleScope(new ScopeChain(m_engine));
            moduleScope->prepend(scope->findNonEmpty(name_productPropertyScope));
            moduleScope->prepend(scope->findNonEmpty(name_projectPropertyScope));
            moduleScope->prepend(childEvObject->scope);
            Property productProperty(evaluationObject);
            childEvObject->scope->properties.insert("product", productProperty);
            Property projectProperty(scope->lookupProperty("project"));
            childEvObject->scope->properties.insert("project", projectProperty);
            evaluateDependencies(child, childEvObject, childScope, moduleScope, userProperties);
        } else if (isArtifact || childPrototypeHash == hashName_Group) {
            // for Group and Artifact, add new module instances
            QHashIterator<QString, Module::Ptr> moduleIt(evaluationObject->modules);
            while (moduleIt.hasNext()) {
                moduleIt.next();
                Module::Ptr module = moduleIt.value();
                if (module->id.isEmpty())
                    continue;

                Scope::Ptr moduleInstance = Scope::create(m_engine,
                                                          m_scopesWithEvaluatedProperties,
                                                          module->object->scope->name(),
                                                          module->file());
                if (!isArtifact)
                    moduleInstance->fallbackScope = module->object->scope;
                moduleInstance->declarations = module->object->scope->declarations;
                insertModulePropertyIntoScope(childEvObject->scope, module, moduleInstance);
            }
        } else if (childPrototypeHash == hashName_Probe) {
            Module::Ptr baseModule = searchAndLoadModule(QStringList("qbs"), QLatin1String("qbs"),
                                                         childScope, userProperties, CodeLocation(object->file->fileName));
            if (!baseModule)
                throw Error(Tr::tr("Cannot load the qbs base module."));
            childEvObject->modules.insert(baseModule->name, baseModule);
            childEvObject->scope->properties.insert(baseModule->id.first(), Property(baseModule->object));
        }

        fillEvaluationObject(childScope, child, ids, childEvObject, userProperties);
        evaluationObject->children.append(childEvObject);
    }

    fillEvaluationObjectBasics(scope, object, evaluationObject);
}

void Loader::LoaderPrivate::fillEvaluationObjectBasics(const ScopeChain::Ptr &scopeChain, LanguageObject *object, EvaluationObject *evaluationObject)
{
    // append the property declarations
    foreach (const PropertyDeclaration &pd, object->propertyDeclarations) {
        PropertyDeclaration &scopePropertyDeclaration = evaluationObject->scope->declarations[pd.name];
        if (!scopePropertyDeclaration.isValid())
            scopePropertyDeclaration = pd;
    }

    applyFunctions(m_engine, object, evaluationObject, scopeChain);
    applyBindings(object, scopeChain);
}

void Loader::LoaderPrivate::setupBuiltinDeclarations()
{
    QList<PropertyDeclaration> decls;
    decls += PropertyDeclaration("name", PropertyDeclaration::String);
    decls += PropertyDeclaration("submodules", PropertyDeclaration::Variant);
    decls += PropertyDeclaration("condition", PropertyDeclaration::Boolean);
    decls += PropertyDeclaration("required", PropertyDeclaration::Boolean);
    decls += PropertyDeclaration("failureMessage", PropertyDeclaration::String);
    foreach (const PropertyDeclaration &pd, decls)
        m_dependsPropertyDeclarations.insert(pd.name, pd);

    decls.clear();
    decls += PropertyDeclaration("condition", PropertyDeclaration::Boolean);
    decls += PropertyDeclaration("name", PropertyDeclaration::String, PropertyDeclaration::PropertyNotAvailableInConfig);
    decls += PropertyDeclaration("files", PropertyDeclaration::Variant, PropertyDeclaration::PropertyNotAvailableInConfig);
    decls += PropertyDeclaration("fileTagsFilter", PropertyDeclaration::Variant, PropertyDeclaration::PropertyNotAvailableInConfig);
    decls += PropertyDeclaration("excludeFiles", PropertyDeclaration::Variant, PropertyDeclaration::PropertyNotAvailableInConfig);
    PropertyDeclaration recursiveProperty("recursive", PropertyDeclaration::Boolean, PropertyDeclaration::PropertyNotAvailableInConfig);
    recursiveProperty.initialValueSource = "false";
    decls += recursiveProperty;
    decls += PropertyDeclaration("fileTags", PropertyDeclaration::Variant, PropertyDeclaration::PropertyNotAvailableInConfig);
    decls += PropertyDeclaration("prefix", PropertyDeclaration::Variant, PropertyDeclaration::PropertyNotAvailableInConfig);

    PropertyDeclaration declaration;
    declaration.name = "overrideTags";
    declaration.type = PropertyDeclaration::Boolean;
    declaration.flags = PropertyDeclaration::PropertyNotAvailableInConfig;
    declaration.initialValueSource = "true";
    decls += declaration;

    foreach (const PropertyDeclaration &pd, decls)
        m_groupPropertyDeclarations.insert(pd.name, pd);

    m_builtinDeclarations.insert(name_Depends, m_dependsPropertyDeclarations.values());
    m_builtinDeclarations.insert(name_Group, m_groupPropertyDeclarations.values());
    PropertyDeclaration conditionProperty("condition", PropertyDeclaration::Boolean);

    QList<PropertyDeclaration> project;
    project += PropertyDeclaration("references", PropertyDeclaration::Variant);
    project += PropertyDeclaration(name_moduleSearchPaths, PropertyDeclaration::Variant);
    m_builtinDeclarations.insert(name_Project, project);

    QList<PropertyDeclaration> product;
    product += PropertyDeclaration("condition", PropertyDeclaration::Boolean);
    product += PropertyDeclaration("type", PropertyDeclaration::String);
    product += PropertyDeclaration("name", PropertyDeclaration::String);
    PropertyDeclaration decl = PropertyDeclaration("targetName", PropertyDeclaration::String);
    decl.initialValueSource = "name";
    product += decl;
    product += PropertyDeclaration("destination", PropertyDeclaration::String);
    product += PropertyDeclaration("consoleApplication", PropertyDeclaration::Boolean);
    product += PropertyDeclaration("files", PropertyDeclaration::Variant, PropertyDeclaration::PropertyNotAvailableInConfig);
    product += PropertyDeclaration("excludeFiles", PropertyDeclaration::Variant, PropertyDeclaration::PropertyNotAvailableInConfig);
    product += PropertyDeclaration("module", PropertyDeclaration::Variant);
    product += PropertyDeclaration("modules", PropertyDeclaration::Variant);
    product += PropertyDeclaration(name_moduleSearchPaths, PropertyDeclaration::Variant);
    m_builtinDeclarations.insert(name_Product, product);

    QList<PropertyDeclaration> fileTagger;
    fileTagger += PropertyDeclaration("pattern", PropertyDeclaration::String);
    fileTagger += PropertyDeclaration("fileTags", PropertyDeclaration::Variant);
    m_builtinDeclarations.insert(name_FileTagger, fileTagger);

    QList<PropertyDeclaration> artifact;
    artifact += conditionProperty;
    artifact += PropertyDeclaration("fileName", PropertyDeclaration::Verbatim);
    artifact += PropertyDeclaration("fileTags", PropertyDeclaration::Variant);
    artifact += PropertyDeclaration("alwaysUpdated", PropertyDeclaration::Boolean);
    m_builtinDeclarations.insert(name_Artifact, artifact);

    QList<PropertyDeclaration> rule;
    rule += PropertyDeclaration("multiplex", PropertyDeclaration::Boolean);
    rule += PropertyDeclaration("inputs", PropertyDeclaration::Variant);
    rule += PropertyDeclaration("usings", PropertyDeclaration::Variant);
    rule += PropertyDeclaration("explicitlyDependsOn", PropertyDeclaration::Variant);
    rule += PropertyDeclaration("prepare", PropertyDeclaration::Verbatim);
    m_builtinDeclarations.insert(name_Rule, rule);

    QList<PropertyDeclaration> transformer;
    transformer += PropertyDeclaration("inputs", PropertyDeclaration::Variant);
    transformer += PropertyDeclaration("prepare", PropertyDeclaration::Verbatim);
    transformer += conditionProperty;
    m_builtinDeclarations.insert(name_Transformer, transformer);

    QList<PropertyDeclaration> productModule;
    m_builtinDeclarations.insert(name_ProductModule, productModule);

    QList<PropertyDeclaration> module;
    module += PropertyDeclaration("name", PropertyDeclaration::String);
    module += PropertyDeclaration("setupBuildEnvironment", PropertyDeclaration::Verbatim);
    module += PropertyDeclaration("setupRunEnvironment", PropertyDeclaration::Verbatim);
    module += PropertyDeclaration("additionalProductFileTags", PropertyDeclaration::Variant);
    module += conditionProperty;
    m_builtinDeclarations.insert(name_Module, module);

    QList<PropertyDeclaration> propertyOptions;
    propertyOptions += PropertyDeclaration("name", PropertyDeclaration::String);
    propertyOptions += PropertyDeclaration("allowedValues", PropertyDeclaration::Variant);
    propertyOptions += PropertyDeclaration("description", PropertyDeclaration::String);
    m_builtinDeclarations.insert(name_PropertyOptions, propertyOptions);

    QList<PropertyDeclaration> probe;
    probe += PropertyDeclaration("condition", PropertyDeclaration::Boolean);
    PropertyDeclaration foundProperty("found", PropertyDeclaration::Boolean);
    foundProperty.initialValueSource = "false";
    probe += foundProperty;
    probe += PropertyDeclaration("configure", PropertyDeclaration::Verbatim);
    m_builtinDeclarations.insert(name_Probe, probe);
}

void Loader::LoaderPrivate::evaluateImports(Scope::Ptr target, const JsImports &jsImports)
{
    QScriptValue importScope;
    for (JsImports::const_iterator it = jsImports.begin(); it != jsImports.end(); ++it) {
        QScriptValue targetObject = m_engine->newObject();
        m_engine->import(*it, importScope, targetObject);
        target->properties.insert(it->scopeName, Property(targetObject.property(it->scopeName)));
    }
}

void Loader::LoaderPrivate::evaluatePropertyOptions(LanguageObject *object)
{
    foreach (LanguageObject *child, object->children) {
        if (child->prototype.last() != name_PropertyOptions)
            continue;

        const Binding nameBinding = child->bindings.value(QStringList("name"));

        if (!nameBinding.isValid())
            throw Error(name_PropertyOptions + " needs to define a 'name'");

        const QScriptValue nameScriptValue = evaluate(m_engine, nameBinding.valueSource);
        const QString name = nameScriptValue.toString();

        if (!object->propertyDeclarations.contains(name))
            throw Error(Tr::tr("no propery with name '%1' found").arg(name));

        PropertyDeclaration &decl = object->propertyDeclarations[name];

        const Binding allowedValuesBinding = child->bindings.value(QStringList("allowedValues"));
        if (allowedValuesBinding.isValid())
            decl.allowedValues = evaluate(m_engine, allowedValuesBinding.valueSource);

        const Binding descriptionBinding = child->bindings.value(QStringList("description"));
        if (descriptionBinding.isValid()) {
            const QScriptValue description = evaluate(m_engine, descriptionBinding.valueSource);
            decl.description = description.toString();
        }
    }
}

static void checkAllowedPropertyValues(const PropertyDeclaration &decl, const QScriptValue &value)
{
    if (!decl.allowedValues.isValid())
        return;

    if (decl.allowedValues.isArray()) {
        QScriptValue length = decl.allowedValues.property("length");
        if (length.isNumber()) {
            const int c = length.toInt32();
            for (int i = 0; i < c; ++i) {
                QScriptValue allowedValue = decl.allowedValues.property(i);
                if (allowedValue.equals(value))
                    return;
            }
        }
    } else if (decl.allowedValues.equals(value)) {
        return;
    }

    QString msg = Tr::tr("value %1 not allowed in property %2").arg(value.toString(), decl.name);
    throw Error(msg);
}

static QString sourceDirPath(const Property &property, const ResolvedProductConstPtr &rproduct)
{
    return property.sourceObject
            ? FileInfo::path(property.sourceObject->file->fileName) : rproduct->sourceDirectory;
}

QVariantMap Loader::LoaderPrivate::evaluateAll(const ResolvedProductConstPtr &rproduct,
                                const Scope::Ptr &properties)
{
    checkCancelation();
    QVariantMap &result = m_convertedScopesCache[properties];
    if (!result.isEmpty())
        return result;

    if (properties->fallbackScope)
        result = evaluateAll(rproduct, properties->fallbackScope);

    typedef QHash<QString, PropertyDeclaration>::const_iterator iter;
    iter end = properties->declarations.end();
    for (iter it = properties->declarations.begin(); it != end; ++it) {
        const PropertyDeclaration &decl = it.value();
        if (decl.type == PropertyDeclaration::Verbatim
            || decl.flags.testFlag(PropertyDeclaration::PropertyNotAvailableInConfig))
        {
            continue;
        }

        Property property = properties->properties.value(it.key());
        if (!property.isValid())
            continue;

        QVariant value;
        if (property.scope) {
            value = evaluateAll(rproduct, property.scope);
        } else {
            QScriptValue scriptValue = properties->property(it.key());
            checkAllowedPropertyValues(decl, scriptValue);
            value = scriptValue.toVariant();
        }

        if (decl.type == PropertyDeclaration::Path) {
            QString fileName = value.toString();
            if (!fileName.isEmpty())
                value = FileInfo::resolvePath(sourceDirPath(property, rproduct), fileName);
        } else if (decl.type == PropertyDeclaration::PathList) {
            QStringList lst = value.toStringList();
            value = resolvePaths(lst, sourceDirPath(property, rproduct));
        }

        result.insert(it.key(), value);
    }
    return result;
}

static Scope::Ptr moduleScopeById(Scope::Ptr scope, const QStringList &moduleId)
{
    for (int i = 0; i < moduleId.count(); ++i) {
        scope = scope->properties.value(moduleId.at(i)).scope;
        if (!scope)
            break;
    }
    return scope;
}

QVariantMap Loader::LoaderPrivate::evaluateModuleValues(const ResolvedProductConstPtr &rproduct,
                                         EvaluationObject *moduleContainer, Scope::Ptr objectScope)
{
    QVariantMap values;
    QVariantMap modules;
    for (QHash<QString, Module::Ptr>::const_iterator it = moduleContainer->modules.begin();
         it != moduleContainer->modules.end(); ++it)
    {
        Module::Ptr module = it.value();
        const QString name = module->name;
        const QStringList id = module->id;
        if (!id.isEmpty()) {
            Scope::Ptr moduleScope = moduleScopeById(objectScope, id);
            if (!moduleScope)
                moduleScope = moduleScopeById(moduleContainer->scope, id);
            if (!moduleScope)
                continue;
            modules.insert(name, evaluateAll(rproduct, moduleScope));
        } else {
            modules.insert(name, evaluateAll(rproduct, module->object->scope));
        }
    }
    values.insert(QLatin1String("modules"), modules);
    return values;
}

Module::Ptr Loader::LoaderPrivate::loadModule(ProjectFile *file, const QStringList &moduleId, const QString &moduleName,
                               ScopeChain::Ptr moduleBaseScope, const QVariantMap &userProperties,
                               const CodeLocation &dependsLocation)
{
    checkCancelation();
    const bool isBaseModule = (moduleName == "qbs");

    Module::Ptr module = Module::Ptr(new Module);
    module->id = moduleId;
    module->name = moduleName;
    module->dependsLocation = dependsLocation;
    module->object = new EvaluationObject(file->root);
    const QString propertiesName = QString("module %1").arg(moduleName);
    module->object->scope = Scope::create(m_engine, m_scopesWithEvaluatedProperties,
                                          propertiesName, file);

    resolveInheritance(file->root, module->object, moduleBaseScope, userProperties);
    if (module->object->prototype != name_Module)
        return Module::Ptr();

    module->object->scope->properties.insert("name", Property(m_engine->toScriptValue(moduleName)));
    module->context = buildFileContext(file);

    ScopeChain::Ptr moduleScope(moduleBaseScope->clone());
    moduleScope->prepend(module->context);
    moduleScope->prepend(module->object->scope);
    if (isBaseModule) {
        // setup helper properties of the base module
        Property p;
        p.value = m_jsFunction_getHostOS;
        module->object->scope->properties.insert("getHostOS", p);
        p.value = m_jsFunction_getHostDefaultArchitecture;
        module->object->scope->properties.insert("getHostDefaultArchitecture", p);
        p.value = m_jsFunction_getenv;
        module->object->scope->properties.insert("getenv", p);
        p.value = m_jsFunction_configurationValue;
        module->object->scope->properties.insert("configurationValue", p);
    }

    evaluatePropertyOptions(file->root);
    evaluateDependencies(file->root, module->object, moduleScope, moduleBaseScope, userProperties, !isBaseModule);
    if (!checkFileCondition(m_engine, moduleScope, file))
        return Module::Ptr();

    qbsTrace() << "loading module '" << moduleName << "' from " << file->fileName;
    buildModulesProperty(module->object);

    if (!file->root->id.isEmpty())
        module->context->properties.insert(file->root->id, Property(module->object));
    fillEvaluationObject(moduleScope, file->root, module->object->scope, module->object, userProperties);
    evaluateDependencyConditions(module->object);

    if (!module->object->unknownModules.isEmpty()) {
        // Some module dependencies could not be resolved.
        // We're deferring the error emission because this module
        // might be removed later due to an unsatisfied dependency condition.
        return module;
    }

    // override properties given on the command line
    const QVariantMap userModuleProperties = userProperties.value(moduleName).toMap();
    for (QVariantMap::const_iterator vmit = userModuleProperties.begin(); vmit != userModuleProperties.end(); ++vmit) {
        if (!module->object->scope->properties.contains(vmit.key()))
            throw Error(Tr::tr("Unknown property: %1.%2").arg(module->id.join("."), vmit.key()));
        module->object->scope->properties.insert(vmit.key(), Property(m_engine->toScriptValue(vmit.value())));
    }

    return module;
}

/**
 * Set up the module and submodule properties in \a targetScope.
 */
void Loader::LoaderPrivate::insertModulePropertyIntoScope(Scope::Ptr targetScope, const Module::Ptr &module, Scope::Ptr moduleInstance)
{
    if (!moduleInstance)
        moduleInstance = module->object->scope;

    for (int i=0; i < module->id.count() - 1; ++i) {
        const QString &currentModuleId = module->id.at(i);
        Scope::Ptr superModuleScope;
        Property p = targetScope->properties.value(currentModuleId);
        if (p.isValid() && p.scope) {
            superModuleScope = p.scope;
        } else {
            ProjectFile *owner = module->object->instantiatingObject()->file;
            superModuleScope = Scope::create(m_engine, m_scopesWithEvaluatedProperties,
                                             currentModuleId, owner);
            superModuleScope->properties.insert("name", Property(m_engine->toScriptValue(currentModuleId)));
            targetScope->insertAndDeclareProperty(currentModuleId, Property(superModuleScope));
        }
        targetScope = superModuleScope;
    }
    targetScope->insertAndDeclareProperty(module->id.last(), Property(moduleInstance));
}

/// load all module.qbs files, checking their conditions
Module::Ptr Loader::LoaderPrivate::searchAndLoadModule(const QStringList &moduleId, const QString &moduleName, ScopeChain::Ptr moduleBaseScope,
                                        const QVariantMap &userProperties, const CodeLocation &dependsLocation,
                                        const QStringList &extraSearchPaths)
{
    Q_ASSERT(!moduleName.isEmpty());
    Module::Ptr module;
    QStringList searchPaths = extraSearchPaths;
    searchPaths.append(m_moduleSearchPaths);

    foreach (const QString &path, searchPaths) {
        QString dirPath = FileInfo::resolvePath(path, moduleName);
        QStringList moduleFileNames = m_moduleDirListCache.value(dirPath);
        if (moduleFileNames.isEmpty()) {
            QString origDirPath = dirPath;
            FileInfo dirInfo(dirPath);
            if (!dirInfo.isDir()) {
                bool found = false;
                if (HostOsInfo::isWindowsHost()) {
                    // On case sensitive file systems try to find the path.
                    QStringList subPaths = moduleName.split("/", QString::SkipEmptyParts);
                    QDir dir(path);
                    if (!dir.cd(moduleSearchSubDir))
                        continue;
                    do {
                        QStringList lst = dir.entryList(QStringList(subPaths.takeFirst()), QDir::Dirs);
                        if (lst.count() != 1)
                            break;
                        if (!dir.cd(lst.first()))
                            break;
                        if (subPaths.isEmpty()) {
                            found = true;
                            dirPath = dir.absolutePath();
                        }
                    } while (!found);
                }
                if (!found)
                    continue;
            }

            QDirIterator dirIter(dirPath, QStringList("*.qbs"));
            while (dirIter.hasNext())
                moduleFileNames += dirIter.next();

            m_moduleDirListCache.insert(origDirPath, moduleFileNames);
        }
        foreach (const QString &fileName, moduleFileNames) {
            ProjectFile::Ptr file = parseFile(fileName);
            if (!file)
                throw Error(Tr::tr("Error while parsing file: %s").arg(fileName), dependsLocation);

            module = loadModule(file.data(), moduleId, moduleName, moduleBaseScope, userProperties, dependsLocation);
            if (module)
                return module;
        }
    }

    return module;
}

void Loader::LoaderPrivate::evaluateDependencies(LanguageObject *object, EvaluationObject *evaluationObject, const ScopeChain::Ptr &localScope,
                                  ScopeChain::Ptr moduleScope, const QVariantMap &userProperties, bool loadBaseModule)
{
    // check for additional module search paths in the product
    QStringList extraSearchPaths;
    Binding searchPathsBinding = object->bindings.value(QStringList(name_moduleSearchPaths));
    if (searchPathsBinding.isValid()) {
        applyBinding(object, searchPathsBinding, localScope);
        const QScriptValue scriptValue = localScope->value().property(name_moduleSearchPaths);
        extraSearchPaths = scriptValue.toVariant().toStringList();
    }

    // ProductModule does not have moduleSearchPaths property
    Property productProperty = localScope->lookupProperty("product");
    if (productProperty.isValid() && productProperty.scope)
        extraSearchPaths = productProperty.scope->stringListValue(name_moduleSearchPaths);
    Property projectProperty = localScope->lookupProperty("project");
    if (projectProperty.isValid() && projectProperty.scope) {
        // if no product.moduleSearchPaths found, check for additional module search paths in the project
        if (extraSearchPaths.isEmpty())
            extraSearchPaths = projectProperty.scope->stringListValue(name_moduleSearchPaths);

        // resolve all found module search paths
        extraSearchPaths = resolvePaths(extraSearchPaths, projectProperty.scope->stringValue("path"));
    }

    if (loadBaseModule) {
        Module::Ptr baseModule = searchAndLoadModule(QStringList("qbs"), QLatin1String("qbs"),
                                                     moduleScope, userProperties, CodeLocation(object->file->fileName));
        if (!baseModule)
            throw Error(Tr::tr("Cannot load the qbs base module."));
        evaluationObject->modules.insert(baseModule->name, baseModule);
        evaluationObject->scope->properties.insert(baseModule->id.first(), Property(baseModule->object));
    }

    foreach (LanguageObject *child, object->children) {
        if (compare(child->prototype, name_Depends)) {
            QList<UnknownModule::Ptr> unknownModules;
            foreach (const Module::Ptr &m, evaluateDependency(child, localScope, moduleScope, extraSearchPaths, &unknownModules, userProperties)) {
                Module::Ptr m2 = evaluationObject->modules.value(m->name);
                if (m2) {
                    m2->dependsCount++;
                    continue;
                }
                m->dependsCount = 1;
                evaluationObject->modules.insert(m->name, m);
                insertModulePropertyIntoScope(evaluationObject->scope, m, m->object->scope);
            }
            evaluationObject->unknownModules.append(unknownModules);
        }
    }
}

void Loader::LoaderPrivate::evaluateDependencyConditions(EvaluationObject *evaluationObject)
{
    bool mustEvaluateConditions = false;
    QVector<Module::Ptr> modules;
    QVector<QSharedPointer<ModuleBase> > moduleBaseObjects;
    foreach (const Module::Ptr &module, evaluationObject->modules) {
        if (!module->condition.isNull())
            mustEvaluateConditions = true;
        modules += module;
        moduleBaseObjects += module;
    }
    foreach (const UnknownModule::Ptr &module, evaluationObject->unknownModules) {
        if (!module->condition.isNull())
            mustEvaluateConditions = true;
        moduleBaseObjects += module;
    }
    if (!mustEvaluateConditions)
        return;

    QScriptContext *context = m_engine->currentContext();
    const QScriptValue activationObject = context->activationObject();

    foreach (const QSharedPointer<ModuleBase> &moduleBaseObject, moduleBaseObjects) {
        if (moduleBaseObject->condition.isNull())
            continue;

        context->setActivationObject(moduleBaseObject->conditionScopeChain->value());
        QScriptValue conditionValue = m_engine->evaluate(moduleBaseObject->condition);
        if (conditionValue.isError()) {
            CodeLocation location(moduleBaseObject->condition.fileName(), moduleBaseObject->condition.firstLineNumber());
            throw Error(Tr::tr("Error while evaluating Depends.condition: %1").arg(conditionValue.toString()), location);
        }
        if (!conditionValue.toBool()) {
            // condition is false, thus remove the module from evaluationObject
            if (Module::Ptr module = moduleBaseObject.dynamicCast<Module>()) {
                if (--module->dependsCount == 0)
                    evaluationObject->modules.remove(module->name);
            } else {
                evaluationObject->unknownModules.removeOne(moduleBaseObject.dynamicCast<UnknownModule>());
            }
        }
    }

    context->setActivationObject(activationObject);
}

void Loader::LoaderPrivate::buildModulesProperty(EvaluationObject *evaluationObject)
{
    // set up a XXX.modules property
    Scope::Ptr modules = Scope::create(m_engine, m_scopesWithEvaluatedProperties,
                                       QLatin1String("modules property"),
                                       evaluationObject->instantiatingObject()->file);
    for (QHash<QString, Module::Ptr>::const_iterator it = evaluationObject->modules.begin();
         it != evaluationObject->modules.end(); ++it)
    {
        modules->properties.insert(it.key(), Property(it.value()->object));
        modules->declarations.insert(it.key(), PropertyDeclaration(it.key(), PropertyDeclaration::Variant));
    }
    evaluationObject->scope->properties.insert("modules", Property(modules));
    evaluationObject->scope->declarations.insert("modules", PropertyDeclaration("modules", PropertyDeclaration::Variant));
}

void Loader::LoaderPrivate::checkModuleDependencies(const Module::Ptr &module)
{
    if (!module->object->unknownModules.isEmpty()) {
        UnknownModule::Ptr missingModule = module->object->unknownModules.first();
        throw Error(Tr::tr("Module '%1' cannot be loaded.").arg(missingModule->name),
                    missingModule->dependsLocation);
    }

    foreach (const Module::Ptr &dependency, module->object->modules)
        checkModuleDependencies(dependency);
}

QList<Module::Ptr> Loader::LoaderPrivate::evaluateDependency(LanguageObject *depends, ScopeChain::Ptr conditionScopeChain,
                                              ScopeChain::Ptr moduleScope, const QStringList &extraSearchPaths,
                                              QList<UnknownModule::Ptr> *unknownModules, const QVariantMap &userProperties)
{
    const CodeLocation dependsLocation = depends->prototypeLocation;

    // check for the use of undeclared properties
    foreach (const Binding &binding, depends->bindings) {
        if (binding.name.count() > 1)
            throw Error(Tr::tr("Bindings with dots are forbidden in Depends."), dependsLocation);
        if (!m_dependsPropertyDeclarations.contains(binding.name.first()))
            throw Error(Tr::tr("There's no property '%1' in Depends.").arg(binding.name.first()),
                               CodeLocation(depends->file->fileName, binding.valueSource.firstLineNumber()));
    }

    QScriptProgram condition;
    Binding binding = depends->bindings.value(QStringList("condition"));
    if (binding.isValid())
        condition = binding.valueSource;

    bool isRequired = true;
    binding = depends->bindings.value(QStringList("required"));
    if (!binding.valueSource.isNull())
        isRequired = evaluate(m_engine, binding.valueSource).toBool();

    QString failureMessage;
    binding = depends->bindings.value(QStringList("failureMessage"));
    if (!binding.valueSource.isNull())
        failureMessage = evaluate(m_engine, binding.valueSource).toString();

    QString moduleName;
    binding = depends->bindings.value(QStringList("name"));
    if (!binding.valueSource.isNull()) {
        moduleName = evaluate(m_engine, binding.valueSource).toString();
    } else {
        moduleName = depends->id;
    }

    QString moduleId = depends->id;
    if (moduleId.isEmpty())
        moduleId = moduleName.toLower();

    QStringList subModules;
    Binding subModulesBinding = depends->bindings.value(QStringList("submodules"));
    if (!subModulesBinding.valueSource.isNull())
        subModules = evaluate(m_engine, subModulesBinding.valueSource).toVariant().toStringList();

    QList<QStringList> fullModuleIds;
    QStringList fullModuleNames;
    if (subModules.isEmpty()) {
        fullModuleIds.append(moduleId.split(QLatin1Char('.')));
        fullModuleNames.append(moduleName.toLower().replace('.', "/"));
    } else {
        foreach (const QString &subModuleName, subModules) {
            fullModuleIds.append(QStringList() << moduleId << subModuleName);
            fullModuleNames.append(moduleName.toLower().replace('.', "/") + "/" + subModuleName.toLower().replace('.', "/"));
        }
    }

    QList<Module::Ptr> modules;
    unknownModules->clear();
    for (int i=0; i < fullModuleNames.count(); ++i) {
        const QString &fullModuleName = fullModuleNames.at(i);
        Module::Ptr module = searchAndLoadModule(fullModuleIds.at(i), fullModuleName, moduleScope, userProperties, dependsLocation, extraSearchPaths);
        ModuleBase::Ptr moduleBase;
        if (module) {
            moduleBase = module;
            modules.append(module);
        } else {
            UnknownModule::Ptr unknownModule(new UnknownModule);
            unknownModule->name = fullModuleName;
            unknownModule->required = isRequired;
            unknownModule->failureMessage = failureMessage;
            unknownModule->dependsLocation = dependsLocation;
            moduleBase = unknownModule;
            unknownModules->append(unknownModule);
        }
        moduleBase->condition = condition;
        moduleBase->conditionScopeChain = conditionScopeChain;
    }
    return modules;
}

static void findModuleDependencies_impl(const Module::Ptr &module, QHash<QString, ProjectFile *> &result)
{
    QString moduleName = module->name;
    ProjectFile *file = module->file();
    ProjectFile *otherFile = result.value(moduleName);
    if (otherFile && otherFile != file) {
        throw Error(Tr::tr("two different versions of '%1' were included: '%2' and '%3'").arg(
                               moduleName, file->fileName, otherFile->fileName));
    } else if (otherFile) {
        return;
    }

    result.insert(moduleName, file);
    foreach (const Module::Ptr &depModule, module->object->modules) {
        findModuleDependencies_impl(depModule, result);
    }
}

static QHash<QString, ProjectFile *> findModuleDependencies(EvaluationObject *root)
{
    QHash<QString, ProjectFile *> result;
    foreach (const Module::Ptr &module, root->modules) {
        foreach (const Module::Ptr &depModule, module->object->modules) {
            findModuleDependencies_impl(depModule, result);
        }
    }
    return result;
}

static void applyFileTaggers(const SourceArtifactPtr &artifact,
        const ResolvedProductConstPtr &product)
{
    if (!artifact->overrideFileTags || artifact->fileTags.isEmpty()) {
        QSet<QString> fileTags = product->fileTagsForFileName(artifact->absoluteFilePath);
        artifact->fileTags.unite(fileTags);
        if (artifact->fileTags.isEmpty())
            artifact->fileTags.insert(QLatin1String("unknown-file-tag"));
        if (qbsLogLevel(LoggerTrace))
            qbsTrace() << "[LDR] adding file tags " << artifact->fileTags << " to " << FileInfo::fileName(artifact->absoluteFilePath);
    }
}

static bool checkCondition(EvaluationObject *object)
{
    QScriptValue scriptValue = object->scope->property("condition");
    if (scriptValue.isString()) {
        // Condition is user-defined
        const QString str = scriptValue.toString();
        if (str == "true")
            return true;
        else if (str == "false")
            return false;
    }
    if (scriptValue.isBool()) {
        return scriptValue.toBool();
    } else if (scriptValue.isValid() && !scriptValue.isUndefined()) {
        const QScriptProgram &scriptProgram = object->objects.first()->bindings.value(QStringList("condition")).valueSource;
        throw Error(Tr::tr("Invalid condition."), CodeLocation(scriptProgram.fileName(), scriptProgram.firstLineNumber()));
    }
    // no 'condition' property means 'the condition is true'
    return true;
}

ResolvedProjectPtr Loader::LoaderPrivate::resolveProject(const QString &buildRoot,
                                                           const QVariantMap &userProperties)
{
    Q_ASSERT(FileInfo::isAbsolute(buildRoot));
    if (qbsLogLevel(LoggerTrace))
        qbsTrace() << "[LDR] resolving " << m_project->fileName;
    ScriptEngineContextPusher contextPusher(m_engine);
    m_scopesWithEvaluatedProperties->clear();
    m_convertedScopesCache.clear();
    ResolvedProjectPtr rproject = ResolvedProject::create();
    rproject->location = CodeLocation(m_project->fileName, 1, 1);
    rproject->setBuildConfiguration(userProperties);
    rproject->buildDirectory = ResolvedProject::deriveBuildDirectory(buildRoot, rproject->id());

    Scope::Ptr context = buildFileContext(m_project.data());
    ScopeChain::Ptr scope(new ScopeChain(m_engine, context));

    ResolvedModulePtr dummyModule = ResolvedModule::create();
    dummyModule->jsImports = m_project->jsImports;
    QList<RulePtr> globalRules;
    QList<FileTagger::ConstPtr> globalFileTaggers;

    ProjectData projectData;
    resolveTopLevel(rproject,
                    m_project->root,
                    m_project->fileName,
                    &projectData,
                    &globalRules,
                    &globalFileTaggers,
                    userProperties,
                    scope,
                    dummyModule);

    ProductMap resolvedProducts;
    QHash<ResolvedProductPtr, ProductData>::iterator it = projectData.products.begin();
    if (m_progressObserver)
        m_progressObserver->initialize(Tr::tr("Loading project"), projectData.products.count());
    for (; it != projectData.products.end(); ++it) {
        if (m_progressObserver) {
            checkCancelation();
            m_progressObserver->incrementProgressValue();
        }
        resolveProduct(it.key(), rproject, it.value(), resolvedProducts, globalRules,
                       globalFileTaggers, dummyModule);
    }

    // Check, if the userProperties contain invalid items.
    checkUserProperties(projectData, userProperties);

    // Check all modules for unresolved dependencies.
    foreach (ResolvedProductPtr rproduct, rproject->products)
        foreach (Module::Ptr module, projectData.products.value(rproduct).product->modules)
            checkModuleDependencies(module);

    // Resolve inter-product dependencies.
    resolveProductDependencies(rproject, projectData, resolvedProducts);

    return rproject;
}

void Loader::LoaderPrivate::resolveModule(ResolvedProductPtr rproduct, const QString &moduleName, EvaluationObject *module)
{
    ResolvedModulePtr rmodule = ResolvedModule::create();
    rmodule->name = moduleName;
    rmodule->jsImports = module->instantiatingObject()->file->jsImports;
    rmodule->setupBuildEnvironmentScript = module->scope->verbatimValue("setupBuildEnvironment");
    rmodule->setupRunEnvironmentScript = module->scope->verbatimValue("setupRunEnvironment");
    QStringList additionalProductFileTags = module->scope->stringListValue("additionalProductFileTags");
    if (!additionalProductFileTags.isEmpty()) {
        rproduct->additionalFileTags.append(additionalProductFileTags);
        rproduct->additionalFileTags = rproduct->additionalFileTags.toSet().toList();
    }
    foreach (Module::Ptr m, module->modules)
        rmodule->moduleDependencies.append(m->name);
    rproduct->modules.append(rmodule);
    QList<EvaluationObject *> unhandledChildren = resolveCommonItems(module->children, rproduct, rmodule);
    if (!unhandledChildren.isEmpty()) {
        EvaluationObject *child = unhandledChildren.first();
        QString msg = Tr::tr("Items of type %0 not allowed in a Module.");
        throw Error(msg.arg(child->prototype));
    }
}

static void createSourceArtifact(const ResolvedProductConstPtr &rproduct,
                                 const PropertyMapPtr &properties,
                                 const QString &fileName,
                                 const QSet<QString> &fileTags,
                                 bool overrideTags,
                                 QList<SourceArtifactPtr> &artifactList)
{
    SourceArtifactPtr artifact = SourceArtifact::create();
    artifact->absoluteFilePath = FileInfo::resolvePath(rproduct->sourceDirectory, fileName);
    artifact->fileTags = fileTags;
    artifact->overrideFileTags = overrideTags;
    artifact->properties = properties;
    artifactList += artifact;
}

/**
  * Resolve Group {} and the files part of Product {}.
  */
void Loader::LoaderPrivate::resolveGroup(ResolvedProductPtr rproduct, EvaluationObject *product, EvaluationObject *group)
{
    const bool isGroup = product != group;

    QStringList files = group->scope->stringListValue("files");
    PropertyMapPtr properties = rproduct->properties;
    if (isGroup) {
        clearScopesCache();

        // Walk through all bindings and check if we set anything
        // that required a separate config for this group.
        bool createNewConfig = false;
        for (int i = 0; i < group->objects.count() && !createNewConfig; ++i) {
            foreach (const Binding &binding, group->objects.at(i)->bindings) {
                if (binding.name.count() > 1 || !m_groupPropertyDeclarations.contains(binding.name.first())) {
                    createNewConfig = true;
                    break;
                }
            }
        }

        if (createNewConfig) {
            // build configuration for this group
            properties = PropertyMap::create();
            properties->setValue(evaluateModuleValues(rproduct, product, group->scope));
        }

        const QStringList fileTagsFilter = group->scope->stringListValue("fileTagsFilter");
        if (!fileTagsFilter.isEmpty()) {
            if (!files.isEmpty())
                throw Error(Tr::tr("Group.files and Group.fileTagsFilters are exclusive."),
                            group->instantiatingObject()->prototypeLocation);
            ArtifactPropertiesPtr aprops = ArtifactProperties::create();
            aprops->setFileTagsFilter(fileTagsFilter);
            QVariantMap cfgval = evaluateModuleValues(rproduct, product, group->scope);
            PropertyMapPtr cfg = PropertyMap::create();
            cfg->setValue(cfgval);
            aprops->setPropertyMap(cfg);
            rproduct->artifactProperties += aprops;
            return;
        }
    }

    // Products can have 'files' but not 'fileTags'
    if (isGroup && files.isEmpty()) {
        // Yield an error if Group without files binding is encountered.
        // An empty files value is OK but a binding must exist.
        bool filesBindingFound = false;
        const QStringList filesBindingName(QStringList() << "files");
        foreach (LanguageObject *obj, group->objects) {
            if (obj->bindings.contains(filesBindingName)) {
                filesBindingFound = true;
                break;
            }
        }
        if (!filesBindingFound)
            throw Error(Tr::tr("Group without files is not allowed."), group->instantiatingObject()->prototypeLocation);
    }
    QStringList patterns;
    QString prefix;
    for (int i = files.count(); --i >= 0;) {
        if (FileInfo::isPattern(files[i]))
            patterns.append(files.takeAt(i));
    }
    if (isGroup) {
        prefix = group->scope->stringValue("prefix");
        if (!prefix.isEmpty()) {
            for (int i = files.count(); --i >= 0;)
                    files[i].prepend(prefix);
        }
    }
    QSet<QString> fileTags;
    if (isGroup)
        fileTags = group->scope->stringListValue("fileTags").toSet();
    bool overrideTags = true;
    if (isGroup)
        overrideTags = group->scope->boolValue("overrideTags", true);

    GroupPtr resolvedGroup = ResolvedGroup::create();
    resolvedGroup->enabled = (!isGroup || checkCondition(group));
    resolvedGroup->location = group->instantiatingObject()->prototypeLocation;

    if (!patterns.isEmpty()) {
        SourceWildCards::Ptr wildcards = SourceWildCards::create();
        if (isGroup) {
            wildcards->recursive = group->scope->boolValue("recursive");
        }
        wildcards->excludePatterns = group->scope->stringListValue("excludeFiles");
        wildcards->prefix = prefix;
        wildcards->patterns = patterns;
        QSet<QString> files = wildcards->expandPatterns(resolvedGroup, rproduct->sourceDirectory);
        foreach (const QString &fileName, files)
            createSourceArtifact(rproduct, properties, fileName,
                                 fileTags, overrideTags, wildcards->files);
        resolvedGroup->wildcards = wildcards;
    }

    foreach (const QString &fileName, files)
        createSourceArtifact(rproduct, properties, fileName,
                             fileTags, overrideTags, resolvedGroup->files);

    resolvedGroup->name = group->scope->stringValue("name");
    if (resolvedGroup->name.isEmpty())
        resolvedGroup->name = Tr::tr("Group %1").arg(rproduct->groups.count());
    resolvedGroup->properties = properties;
    rproduct->groups += resolvedGroup;
}

void Loader::LoaderPrivate::resolveProductModule(const ResolvedProductConstPtr &rproduct,
        EvaluationObject *productModule)
{
    Q_ASSERT(!rproduct->name.isEmpty());

    evaluateDependencyConditions(productModule);
    clearScopesCache();
    QVariantMap moduleValues = evaluateModuleValues(rproduct, productModule, productModule->scope);
    m_productModules.insert(rproduct->name.toLower(), moduleValues);
}

void Loader::LoaderPrivate::resolveTransformer(ResolvedProductPtr rproduct, EvaluationObject *trafo, ResolvedModuleConstPtr module)
{
    if (!checkCondition(trafo))
        return;

    ResolvedTransformer::Ptr rtrafo = ResolvedTransformer::create();
    rtrafo->module = module;
    rtrafo->jsImports = trafo->instantiatingObject()->file->jsImports;
    rtrafo->inputs = trafo->scope->stringListValue("inputs");
    for (int i=0; i < rtrafo->inputs.count(); ++i)
        rtrafo->inputs[i] = FileInfo::resolvePath(rproduct->sourceDirectory, rtrafo->inputs[i]);
    const PrepareScriptPtr transform = PrepareScript::create();
    transform->script = trafo->scope->verbatimValue("prepare");
    transform->location.fileName = trafo->instantiatingObject()->file->fileName;
    transform->location.column = 1;
    Binding binding = trafo->instantiatingObject()->bindings.value(QStringList("prepare"));
    transform->location.line = binding.valueSource.firstLineNumber();
    rtrafo->transform = transform;

    foreach (EvaluationObject *child, trafo->children) {
        if (child->prototype != name_Artifact)
            throw Error(Tr::tr("Transformer: wrong child type '%0'.").arg(child->prototype));
        SourceArtifactPtr artifact = SourceArtifact::create();
        artifact->properties = rproduct->properties;
        QString fileName = child->scope->stringValue("fileName");
        if (fileName.isEmpty())
            throw Error(Tr::tr("Artifact fileName must not be empty."));
        artifact->absoluteFilePath = FileInfo::resolvePath(rproduct->project->buildDirectory,
                                                           fileName);
        artifact->fileTags = child->scope->stringListValue("fileTags").toSet();
        rtrafo->outputs += artifact;
    }
    rproduct->transformers += rtrafo;
}

void Loader::LoaderPrivate::resolveProbe(EvaluationObject *node)
{
    if (!checkCondition(node))
        return;
    if (!m_probeScriptScope.isValid()) {
        m_probeScriptScope = m_engine->newObject();
        File::init(m_probeScriptScope);
        TextFile::init(m_probeScriptScope);
        Process::init(m_probeScriptScope);
    }
    QScriptContext *ctx = m_engine->pushContext();
    ctx->pushScope(m_probeScriptScope);
    ProbeScope::Ptr scope = ProbeScope::create(m_engine, node->scope);
    ctx->setActivationObject(scope->value());
    for (int i = node->objects.size() - 1; i >= 0; --i)
        applyFunctions(m_engine, node->objects.at(i), node);
    QString constructor = node->scope->verbatimValue("configure");
    evaluate(m_engine, constructor);
    ctx->popScope();
    m_engine->popContext();
}

/**
 *Resolve stuff that Module and Product have in common.
 */
QList<EvaluationObject *> Loader::LoaderPrivate::resolveCommonItems(const QList<EvaluationObject *> &objects,
                                                        ResolvedProductPtr rproduct, const ResolvedModuleConstPtr &module)
{
    QList<RulePtr> rules;

    QList<EvaluationObject *> unhandledObjects;
    foreach (EvaluationObject *object, objects) {
        const uint hashPrototypeName = qHash(object->prototype);
        if (hashPrototypeName == hashName_FileTagger) {
            rproduct->fileTaggers.insert(resolveFileTagger(object));
        } else if (hashPrototypeName == hashName_Rule) {
            const RulePtr rule = resolveRule(object, module);
            rproduct->rules.insert(rule);
            rules.append(rule);
        } else if (hashPrototypeName == hashName_Transformer) {
            resolveTransformer(rproduct, object, module);
        } else if (hashPrototypeName == hashName_PropertyOptions) {
            // Just ignore this type to allow it. It is handled elsewhere.
        } else if (hashPrototypeName == hashName_Probe) {
            resolveProbe(object);
        } else {
            unhandledObjects.append(object);
        }
    }

    return unhandledObjects;
}

RulePtr Loader::LoaderPrivate::resolveRule(EvaluationObject *object, ResolvedModuleConstPtr module)
{
    RulePtr rule = Rule::create();

    // read artifacts
    QList<RuleArtifactConstPtr> artifacts;
    bool hasAlwaysUpdatedArtifact = false;
    foreach (EvaluationObject *child, object->children) {
        const uint hashChildPrototypeName = qHash(child->prototype);
        if (hashChildPrototypeName == hashName_Artifact) {
            RuleArtifactPtr artifact = RuleArtifact::create();
            artifacts.append(artifact);
            artifact->fileName = child->scope->verbatimValue("fileName");
            artifact->fileTags = child->scope->stringListValue("fileTags");
            artifact->alwaysUpdated = child->scope->boolValue("alwaysUpdated", true);
            if (artifact->alwaysUpdated)
                hasAlwaysUpdatedArtifact = true;
            LanguageObject *origArtifactObj = child->instantiatingObject();
            foreach (const Binding &binding, origArtifactObj->bindings) {
                if (binding.name.length() <= 1)
                    continue;
                RuleArtifact::Binding rabinding;
                rabinding.name = binding.name;
                rabinding.code = binding.valueSource.sourceCode();
                rabinding.location.fileName = binding.valueSource.fileName();
                rabinding.location.line = binding.valueSource.firstLineNumber();
                artifact->bindings.append(rabinding);
            }
        } else {
            throw Error(Tr::tr("'Rule' can only have children of type 'Artifact'."),
                               child->instantiatingObject()->prototypeLocation);
        }
    }

    if (!hasAlwaysUpdatedArtifact)
        throw Error(Tr::tr("At least one output artifact of a rule "
                           "must have alwaysUpdated set to true."),
                    object->instantiatingObject()->prototypeLocation);

    const PrepareScriptPtr prepareScript = PrepareScript::create();
    prepareScript->script = object->scope->verbatimValue("prepare");
    prepareScript->location.fileName = object->instantiatingObject()->file->fileName;
    prepareScript->location.column = 1;
    {
        Binding binding = object->instantiatingObject()->bindings.value(QStringList("prepare"));
        prepareScript->location.line = binding.valueSource.firstLineNumber();
    }

    rule->jsImports = object->instantiatingObject()->file->jsImports;
    rule->script = prepareScript;
    rule->artifacts = artifacts;
    rule->multiplex = object->scope->boolValue("multiplex", false);
    rule->inputs = object->scope->stringListValue("inputs");
    rule->usings = object->scope->stringListValue("usings");
    rule->explicitlyDependsOn = object->scope->stringListValue("explicitlyDependsOn");
    rule->module = module;

    return rule;
}

FileTagger::ConstPtr Loader::LoaderPrivate::resolveFileTagger(EvaluationObject *evaluationObject)
{
    const Scope::Ptr scope = evaluationObject->scope;
    return FileTagger::create(QRegExp(scope->stringValue("pattern")),
            scope->stringListValue("fileTags"));
}

/// --------------------------------------------------------------------------


template <typename T>
static QString textOf(const QString &source, T *node)
{
    if (!node)
        return QString();
    return source.mid(node->firstSourceLocation().begin(), node->lastSourceLocation().end() - node->firstSourceLocation().begin());
}

static void bindFunction(LanguageObject *result, const QString &source, FunctionDeclaration *ast)
{
    Function f;
    if (ast->name.isNull())
        throw Error(Tr::tr("function decl without name"));
    f.name = ast->name.toString();

    // remove the name
    QString funcNoName = textOf(source, ast);
    funcNoName.replace(QRegExp("^(\\s*function\\s*)\\w*"), "(\\1");
    funcNoName.append(")");
    f.source = QScriptProgram(funcNoName, result->file->fileName, ast->firstSourceLocation().startLine);
    result->functions.insert(f.name, f);
}

static QStringList toStringList(UiQualifiedId *qid)
{
    QStringList result;
    for (; qid; qid = qid->next) {
        if (!qid->name.isEmpty())
            result.append(qid->name.toString());
        else
            result.append(QString()); // throw error instead?
    }
    return result;
}

static CodeLocation toCodeLocation(const QString &fileName, SourceLocation location)
{
    return CodeLocation(fileName, location.startLine, location.startColumn);
}

static QScriptProgram bindingProgram(const QString &fileName, const QString &source, Statement *statement)
{
    QString sourceCode = textOf(source, statement);
    if (cast<Block *>(statement)) {
        // rewrite blocks to be able to use return statements in property assignments
        sourceCode.prepend("(function()");
        sourceCode.append(")()");
    }
    return QScriptProgram(sourceCode, fileName,
                          statement->firstSourceLocation().startLine);
}

static void checkDuplicateBinding(LanguageObject *object, const QStringList &bindingName, const SourceLocation &sourceLocation)
{
    if (object->bindings.contains(bindingName)) {
        QString msg = Tr::tr("Duplicate binding for '%1'");
        throw Error(msg.arg(bindingName.join(".")),
                    toCodeLocation(object->file->fileName, sourceLocation));
    }
}

static void bindBinding(LanguageObject *result, const QString &source, UiScriptBinding *ast)
{
    Binding p;
    if (!ast->qualifiedId || ast->qualifiedId->name.isEmpty())
        throw Error(Tr::tr("script binding without name"));
    p.name = toStringList(ast->qualifiedId);
    checkDuplicateBinding(result, p.name, ast->qualifiedId->identifierToken);

    if (p.name == QStringList("id")) {
        ExpressionStatement *expStmt = cast<ExpressionStatement *>(ast->statement);
        if (!expStmt)
            throw Error(Tr::tr("id: must be followed by identifier"));
        IdentifierExpression *idExp = cast<IdentifierExpression *>(expStmt->expression);
        if (!idExp || idExp->name.isEmpty())
            throw Error(Tr::tr("id: must be followed by identifier"));
        result->id = idExp->name.toString();
        return;
    }

    p.valueSource = bindingProgram(result->file->fileName, source, ast->statement);

    IdentifierSearch idsearch;
    idsearch.add(QLatin1String("base"), &p.valueSourceUsesBase);
    idsearch.add(QLatin1String("outer"), &p.valueSourceUsesOuter);
    idsearch.start(ast->statement);

    result->bindings.insert(p.name, p);
}

static void bindBinding(LanguageObject *result, const QString &source, UiPublicMember *ast)
{
    Binding p;
    if (ast->name.isNull())
        throw Error(Tr::tr("public member without name"));
    p.name = QStringList(ast->name.toString());
    checkDuplicateBinding(result, p.name, ast->identifierToken);

    if (ast->statement) {
        p.valueSource = bindingProgram(result->file->fileName, source, ast->statement);
        IdentifierSearch idsearch;
        idsearch.add(QLatin1String("base"), &p.valueSourceUsesBase);
        idsearch.add(QLatin1String("outer"), &p.valueSourceUsesOuter);
        idsearch.start(ast->statement);
    } else {
        p.valueSource = QScriptProgram("undefined");
    }

    result->bindings.insert(p.name, p);
}

static PropertyDeclaration::Type propertyTypeFromString(const QString &typeName)
{
    if (typeName == "bool")
        return PropertyDeclaration::Boolean;
    if (typeName == "path")
        return PropertyDeclaration::Path;
    if (typeName == "pathList")
        return PropertyDeclaration::PathList;
    if (typeName == "string")
        return PropertyDeclaration::String;
    if (typeName == "var" || typeName == "variant")
        return PropertyDeclaration::Variant;
    return PropertyDeclaration::UnknownType;
}

static void bindPropertyDeclaration(LanguageObject *result, UiPublicMember *ast)
{
    PropertyDeclaration p;
    if (ast->name.isEmpty())
        throw Error(Tr::tr("public member without name"));
    if (ast->memberType.isEmpty())
        throw Error(Tr::tr("public member without type"));
    if (ast->type == UiPublicMember::Signal)
        throw Error(Tr::tr("public member with signal type not supported"));
    p.name = ast->name.toString();
    p.type = propertyTypeFromString(ast->memberType.toString());
    if (ast->typeModifier.compare(QLatin1String("list")))
        p.flags |= PropertyDeclaration::ListProperty;
    else if (!ast->typeModifier.isEmpty())
        throw Error(Tr::tr("public member with type modifier '%1' not supported").arg(ast->typeModifier.toString()));

    result->propertyDeclarations.insert(p.name, p);
}

static LanguageObject *bindObject(ProjectFile::Ptr file, const QString &source, UiObjectDefinition *ast,
                              const QHash<QStringList, QString> prototypeToFile)
{
    LanguageObject *result = new LanguageObject(file.data());
    result->file = file.data();

//    result->location = CodeLocation(
//            currentSourceFileName,
//            ast->firstSourceLocation().startLine,
//            ast->firstSourceLocation().startColumn
//    );

    if (!ast->qualifiedTypeNameId || ast->qualifiedTypeNameId->name.isEmpty())
        throw Error(Tr::tr("no prototype"));
    result->prototype = toStringList(ast->qualifiedTypeNameId);
    result->prototypeLocation = toCodeLocation(file->fileName, ast->qualifiedTypeNameId->identifierToken);

    // resolve prototype if possible
    result->prototypeFileName = prototypeToFile.value(result->prototype);

    if (!ast->initializer)
        return result;

    for (UiObjectMemberList *it = ast->initializer->members; it; it = it->next) {
        if (UiPublicMember *publicMember = cast<UiPublicMember *>(it->member)) {
            bindPropertyDeclaration(result, publicMember);
            bindBinding(result, source, publicMember);
        } else if (UiScriptBinding *scriptBinding = cast<UiScriptBinding *>(it->member)) {
            bindBinding(result, source, scriptBinding);
        } else if (UiSourceElement *sourceElement = cast<UiSourceElement *>(it->member)) {
           FunctionDeclaration *fdecl = cast<FunctionDeclaration *>(sourceElement->sourceElement);
           if (!fdecl)
               continue;
           bindFunction(result, source, fdecl);
        } else if (UiObjectDefinition *objDef = cast<UiObjectDefinition *>(it->member)) {
            result->children += bindObject(file, source, objDef, prototypeToFile);
        }
    }

    return result;
}

static void collectPrototypes(const QString &path, const QString &as,
                              QHash<QStringList, QString> *prototypeToFile)
{
    QDirIterator dirIter(path, QStringList("*.qbs"));
    while (dirIter.hasNext()) {
        const QString filePath = dirIter.next();
        const QString fileName = dirIter.fileName();

        if (fileName.size() <= 4)
            continue;

        const QString componentName = fileName.left(fileName.size() - 4);
        // ### validate componentName

        if (!componentName.at(0).isUpper())
            continue;

        QStringList prototypeName;
        if (!as.isEmpty())
            prototypeName.append(as);
        prototypeName.append(componentName);

        prototypeToFile->insert(prototypeName, filePath);
    }
}

static ProjectFile::Ptr bindFile(const QString &source, const QString &fileName, UiProgram *ast, QStringList searchPaths)
{
    ProjectFile::Ptr file(new ProjectFile);
    file->fileName = fileName;
    const QString path = FileInfo::path(fileName);

    // maps names of known prototypes to absolute file names
    QHash<QStringList, QString> prototypeToFile;

    // files in the same directory are available as prototypes
    collectPrototypes(path, QString(), &prototypeToFile);

    QSet<QString> importAsNames;
    QHash<QString, JsImport> jsImports;

    for (const QbsQmlJS::AST::UiImportList *it = ast->imports; it; it = it->next) {
        const QbsQmlJS::AST::UiImport *const import = it->import;

        QStringList importUri;
        bool isBase = false;
        if (import->importUri) {
            importUri = toStringList(import->importUri);
            if (importUri.size() == 2
                    && importUri.first() == "qbs"
                    && importUri.last() == "base")
                isBase = true; // ### check version!
        }

        QString as;
        if (isBase) {
            if (!import->importId.isNull()) {
                throw Error(Tr::tr("Import of qbs.base must have no 'as <Name>'"),
                            toCodeLocation(fileName, import->importIdToken));
            }
        } else {
            if (import->importId.isNull()) {
                throw Error(Tr::tr("Imports require 'as <Name>'"),
                            toCodeLocation(fileName, import->importToken));
            }

            as = import->importId.toString();
            if (importAsNames.contains(as)) {
                throw Error(Tr::tr("Can't import into the same name more than once."),
                            toCodeLocation(fileName, import->importIdToken));
            }
            importAsNames.insert(as);
        }

        if (!import->fileName.isNull()) {
            QString name = FileInfo::resolvePath(path, import->fileName.toString());

            QFileInfo fi(name);
            if (!fi.exists())
                throw Error(Tr::tr("Can't find imported file %0.").arg(name),
                            CodeLocation(fileName, import->fileNameToken.startLine, import->fileNameToken.startColumn));
            name = fi.canonicalFilePath();
            if (fi.isDir()) {
                collectPrototypes(name, as, &prototypeToFile);
            } else {
                if (name.endsWith(".js", Qt::CaseInsensitive)) {
                    JsImport &jsImport = jsImports[as];
                    jsImport.scopeName = as;
                    jsImport.fileNames.append(name);
                    jsImport.location = toCodeLocation(fileName, import->firstSourceLocation());
                } else if (name.endsWith(".qbs", Qt::CaseInsensitive)) {
                    prototypeToFile.insert(QStringList(as), name);
                } else {
                    throw Error(Tr::tr("Can only import .qbs and .js files"),
                                CodeLocation(fileName, import->fileNameToken.startLine, import->fileNameToken.startColumn));
                }
            }
        } else if (!importUri.isEmpty()) {
            const QString importPath = importUri.join(QDir::separator());
            bool found = false;
            foreach (const QString &searchPath, searchPaths) {
                const QFileInfo fi(FileInfo::resolvePath(FileInfo::resolvePath(searchPath, "imports"), importPath));
                if (fi.isDir()) {
                    // ### versioning, qbsdir file, etc.
                    const QString &resultPath = fi.absoluteFilePath();
                    collectPrototypes(resultPath, as, &prototypeToFile);

                    QDirIterator dirIter(resultPath, QStringList("*.js"));
                    while (dirIter.hasNext()) {
                        dirIter.next();
                        JsImport &jsImport = jsImports[as];
                        if (jsImport.scopeName.isNull()) {
                            jsImport.scopeName = as;
                            jsImport.location = toCodeLocation(fileName, import->firstSourceLocation());
                        }
                        jsImport.fileNames.append(dirIter.filePath());
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw Error(Tr::tr("import %1 not found").arg(importUri.join(".")),
                            toCodeLocation(fileName, import->fileNameToken));
            }
        }
    }

    UiObjectDefinition *rootDef = cast<UiObjectDefinition *>(ast->members->member);
    if (rootDef)
        file->root = bindObject(file, source, rootDef, prototypeToFile);

    file->jsImports = jsImports.values();
    return file;
}

ProjectFile::Ptr Loader::LoaderPrivate::parseFile(const QString &fileName)
{
    ProjectFile::Ptr result = m_parsedFiles.value(fileName);
    if (result)
        return result;

    QFile file(fileName);
    if (!file.open(QFile::ReadOnly))
        throw Error(Tr::tr("Couldn't open '%1'.").arg(fileName));

    const QString code = QTextStream(&file).readAll();
    QScopedPointer<QbsQmlJS::Engine> engine(new QbsQmlJS::Engine);
    QbsQmlJS::Lexer lexer(engine.data());
    lexer.setCode(code, 1);
    QbsQmlJS::Parser parser(engine.data());
    parser.parse();

    QList<QbsQmlJS::DiagnosticMessage> parserMessages = parser.diagnosticMessages();
    if (!parserMessages.isEmpty()) {
        Error err;
        foreach (const QbsQmlJS::DiagnosticMessage &msg, parserMessages)
            err.append(msg.message, CodeLocation(fileName, msg.loc.startLine, msg.loc.startColumn));
        throw err;
    }

    result = bindFile(code, fileName, parser.ast(), m_searchPaths);
    if (result) {
        Q_ASSERT(!m_parsedFiles.contains(result->fileName));
        m_parsedFiles.insert(result->fileName, result);
    }
    return result;
}

void Loader::LoaderPrivate::checkCancelation() const
{
    if (m_progressObserver && m_progressObserver->canceled())
        throw Error(Tr::tr("Loading canceled."));
}

Loader::LoaderPrivate *Loader::LoaderPrivate::get(QScriptEngine *engine)
{
    QVariant v = engine->property(szLoaderPropertyName);
    return static_cast<LoaderPrivate*>(v.value<void*>());
}

QScriptValue Loader::LoaderPrivate::js_getHostOS(QScriptContext *context, QScriptEngine *engine)
{
    Q_UNUSED(context);
    QString hostSystem;

    // TODO: Do we really not support other UNIX systems?
#if defined(Q_OS_WIN)
    hostSystem = "windows";
#elif defined(Q_OS_MAC)
    hostSystem = "mac";
#elif defined(Q_OS_LINUX)
    hostSystem = "linux";
#else
#   error unknown host platform
#endif
    return engine->toScriptValue(hostSystem);
}

QScriptValue Loader::LoaderPrivate::js_getHostDefaultArchitecture(QScriptContext *context, QScriptEngine *engine)
{
    // ### TODO implement properly, do not hard-code
    Q_UNUSED(context);
    QString architecture;
#if defined(Q_OS_WIN)
    architecture = "x86";
#elif defined(Q_OS_MAC)
    architecture = "x86_64";
#elif defined(Q_OS_LINUX)
    architecture = "x86_64";
#else
#   error unknown host platform
#endif
    return engine->toScriptValue(architecture);
}

QScriptValue Loader::LoaderPrivate::js_getenv(QScriptContext *context, QScriptEngine *engine)
{
    if (context->argumentCount() < 1) {
        return context->throwError(QScriptContext::SyntaxError,
                                   QLatin1String("getenv expects 1 argument"));
    }
    const QByteArray name = context->argument(0).toString().toLocal8Bit();
    const QByteArray value = qgetenv(name);
    return value.isNull() ? engine->undefinedValue() : QString::fromLocal8Bit(value);
}

QScriptValue Loader::LoaderPrivate::js_configurationValue(QScriptContext *context, QScriptEngine *engine)
{
    if (context->argumentCount() < 1 || context->argumentCount() > 2) {
        return context->throwError(QScriptContext::SyntaxError,
                                   QString("configurationValue expects 1 or 2 arguments"));
    }

    const Settings &settings = get(engine)->m_settings;
    const bool defaultValueProvided = context->argumentCount() > 1;
    const QString key = context->argument(0).toString();
    QString defaultValue;
    if (defaultValueProvided) {
        const QScriptValue arg = context->argument(1);
        if (!arg.isUndefined())
            defaultValue = arg.toString();
    }
    QVariant v = settings.value(key, defaultValue);
    if (!defaultValueProvided && v.isNull())
        return context->throwError(QScriptContext::SyntaxError,
                                   QString("configuration value '%1' does not exist").arg(context->argument(0).toString()));
    return engine->toScriptValue(v);
}

void Loader::LoaderPrivate::resolveTopLevel(const ResolvedProjectPtr &rproject,
                             LanguageObject *object,
                             const QString &projectFileName,
                             ProjectData *projectData,
                             QList<RulePtr> *globalRules,
                             QList<FileTagger::ConstPtr> *globalFileTaggers,
                             const QVariantMap &userProperties,
                             const ScopeChain::Ptr &scope,
                             const ResolvedModuleConstPtr &dummyModule)
{
    if (qbsLogLevel(LoggerTrace))
        qbsTrace() << "[LDR] resolve top-level " << object->file->fileName;
    EvaluationObject *evaluationObject = new EvaluationObject(object);

    const QString propertiesName = object->prototype.join(".");
    evaluationObject->scope = Scope::create(m_engine, m_scopesWithEvaluatedProperties,
                                            propertiesName, object->file);

    Scope::Ptr productProperty = Scope::create(m_engine, m_scopesWithEvaluatedProperties,
                                               name_productPropertyScope, object->file);
    Scope::Ptr projectProperty = Scope::create(m_engine, m_scopesWithEvaluatedProperties,
                                               name_projectPropertyScope, object->file);
    Scope::Ptr oldProductProperty(scope->findNonEmpty(name_productPropertyScope));
    Scope::Ptr oldProjectProperty(scope->findNonEmpty(name_projectPropertyScope));

    // for the 'product' and 'project' property available to the modules
    ScopeChain::Ptr moduleScope(new ScopeChain(m_engine));
    moduleScope->prepend(oldProductProperty);
    moduleScope->prepend(oldProjectProperty);
    moduleScope->prepend(productProperty);
    moduleScope->prepend(projectProperty);

    ScopeChain::Ptr localScope(scope->clone());
    localScope->prepend(productProperty);
    localScope->prepend(projectProperty);
    localScope->prepend(evaluationObject->scope);

    evaluationObject->scope->properties.insert("configurationValue", Property(m_jsFunction_configurationValue));

    resolveInheritance(object, evaluationObject, moduleScope, userProperties);

    if (evaluationObject->prototype == name_Project) {
        // if this is a nested project, set a fallback scope
        Property outerProperty = scope->lookupProperty("project");
        if (outerProperty.isValid() && outerProperty.scope) {
            evaluationObject->scope->fallbackScope = outerProperty.scope;
        } else {
            // set the 'path' and 'filePath' properties
            setPathAndFilePath(evaluationObject->scope, object->file->fileName);
        }

        fillEvaluationObjectBasics(localScope, object, evaluationObject);

        // override properties from command line
        const QVariantMap overriddenProperties = userProperties.value("project").toMap();
        for (QVariantMap::const_iterator it = overriddenProperties.begin(); it != overriddenProperties.end(); ++it) {
            if (!evaluationObject->scope->properties.contains(it.key()))
                throw Error(Tr::tr("No property '%1' in project scope.").arg(it.key()));
            evaluationObject->scope->properties.insert(it.key(), Property(m_engine->toScriptValue(it.value())));
        }

        // load referenced files
        foreach (const QString &reference, evaluationObject->scope->stringListValue("references")) {
            QString projectFileDir = FileInfo::path(projectFileName);
            QString absReferencePath = FileInfo::resolvePath(projectFileDir, reference);
            ProjectFile::Ptr referencedFile = parseFile(absReferencePath);
            ScopeChain::Ptr referencedFileScope(new ScopeChain(m_engine, buildFileContext(referencedFile.data())));
            referencedFileScope->prepend(projectProperty);
            resolveTopLevel(rproject,
                            referencedFile->root,
                            referencedFile->fileName,
                            projectData,
                            globalRules,
                            globalFileTaggers,
                            userProperties,
                            referencedFileScope,
                            dummyModule);
        }

        // load children
        ScopeChain::Ptr childScope(scope->clone()->prepend(projectProperty));
        foreach (LanguageObject *child, object->children)
            resolveTopLevel(rproject,
                            child,
                            projectFileName,
                            projectData,
                            globalRules,
                            globalFileTaggers,
                            userProperties,
                            childScope,
                            dummyModule);

        return;
    } else if (evaluationObject->prototype == name_Rule) {
        fillEvaluationObject(localScope, object, evaluationObject->scope, evaluationObject, userProperties);
        globalRules->append(resolveRule(evaluationObject, dummyModule));

        return;
    } else if (evaluationObject->prototype == name_FileTagger) {
        fillEvaluationObject(localScope, object, evaluationObject->scope, evaluationObject, userProperties);
        globalFileTaggers->append(resolveFileTagger(evaluationObject));

        return;
    } else if (evaluationObject->prototype != name_Product) {
        QString msg = Tr::tr("'%1' is not allowed here.");
        throw Error(msg.arg(evaluationObject->prototype),
                    evaluationObject->instantiatingObject()->prototypeLocation);
    }

    // set the 'path' and 'filePath' properties
    setPathAndFilePath(evaluationObject->scope, object->file->fileName);

    const ResolvedProductPtr rproduct = ResolvedProduct::create();
    rproduct->location = evaluationObject->instantiatingObject()->prototypeLocation;
    rproduct->sourceDirectory = QFileInfo(projectFileName).absolutePath();
    rproduct->project = rproject;

    ProductData productData;
    productData.product = evaluationObject;

    evaluateDependencies(object, evaluationObject, localScope, moduleScope, userProperties);
    fillEvaluationObject(localScope, object, evaluationObject->scope, evaluationObject, userProperties);
    evaluateDependencyConditions(evaluationObject);
    productData.usedProducts = evaluationObject->unknownModules;

    // check if product's name is empty and set a default value
    Property &nameProperty = evaluationObject->scope->properties["name"];
    if (nameProperty.value.isUndefined()) {
        QString projectName = FileInfo::fileName(projectFileName);
        projectName.chop(4);
        nameProperty.value = m_engine->toScriptValue(projectName);
    }

    // override properties from command line
    productData.originalProductName = evaluationObject->scope->stringValue("name");
    const QVariantMap overriddenProperties = userProperties.value(productData.originalProductName).toMap();
    for (QVariantMap::const_iterator it = overriddenProperties.begin(); it != overriddenProperties.end(); ++it) {
        if (!evaluationObject->scope->properties.contains(it.key()))
            throw Error(Tr::tr("No property '%1' in product '%2'.").arg(it.key(), productData.originalProductName));
        evaluationObject->scope->properties.insert(it.key(), Property(m_engine->toScriptValue(it.value())));
    }

    if (!object->id.isEmpty()) {
        Scope::Ptr context = scope->last();
        context->properties.insert(object->id, Property(evaluationObject));
    }

    // collect all dependent modules and put them into the product
    QHash<QString, ProjectFile *> moduleDependencies = findModuleDependencies(evaluationObject);
    QHashIterator<QString, ProjectFile *> it(moduleDependencies);
    while (it.hasNext()) {
        it.next();

        Module::Ptr moduleInProduct = productData.product->modules.value(it.key());
        if (moduleInProduct) {
            if (moduleInProduct->file() != it.value())
                throw Error(Tr::tr("two different versions of '%1' were used: '%2' and '%3'").arg(
                                it.key(), it.value()->fileName, moduleInProduct->file()->fileName));
            continue;
        }

        Module::Ptr module = loadModule(it.value(), QStringList(), it.key(), moduleScope, userProperties,
                                        CodeLocation(object->file->fileName));
        if (!module) {
            throw Error(Tr::tr("could not load module '%1' from file '%2' into product even though it was loaded into a submodule").arg(
                                   it.key(), it.value()->fileName));
        }
        evaluationObject->modules.insert(module->name, module);
    }
    buildModulesProperty(evaluationObject);

    // check the product's condition
    rproduct->enabled = checkCondition(evaluationObject);
    if (!rproduct->enabled && qbsLogLevel(LoggerTrace))
        qbsTrace() << "[LDR] condition for product '" << rproduct->name << "' is false.";

    projectData->products.insert(rproduct, productData);
}

void Loader::LoaderPrivate::checkUserProperties(const Loader::LoaderPrivate::ProjectData &projectData,
                                 const QVariantMap &userProperties)
{
    QSet<QString> allowedUserPropertyNames;
    allowedUserPropertyNames << QLatin1String("project");
    for (QHash<ResolvedProductPtr, ProductData>::const_iterator it = projectData.products.constBegin();
         it != projectData.products.constEnd(); ++it)
    {
        const ResolvedProductPtr &product = it.key();
        const ProductData &productData = it.value();
        allowedUserPropertyNames += product->name;
        allowedUserPropertyNames += productData.originalProductName;
        foreach (const ResolvedModuleConstPtr &module, product->modules) {
            allowedUserPropertyNames += module->name;
            foreach (const QString &dependency, module->moduleDependencies)
                allowedUserPropertyNames += dependency;
        }
    }

    for (QVariantMap::const_iterator it = userProperties.begin(); it != userProperties.end(); ++it) {
        const QString &propertyName = it.key();
        if (allowedUserPropertyNames.contains(propertyName))
            continue;
        if (existsModuleInSearchPath(propertyName))
            continue;
        throw Error(Tr::tr("%1 is not a product or a module.").arg(propertyName));
    }
}

void Loader::LoaderPrivate::resolveProduct(const ResolvedProductPtr &rproduct,
        const ResolvedProjectPtr &project, ProductData &data, Loader::LoaderPrivate::ProductMap &products,
        const QList<RulePtr> &globalRules, const QList<FileTagger::ConstPtr> &globalFileTaggers,
        const ResolvedModuleConstPtr &dummyModule)
{
    Scope *productProps = data.product->scope.data();

    rproduct->name = productProps->stringValue("name");
    rproduct->targetName = productProps->stringValue("targetName");
    rproduct->properties = PropertyMap::create();

    // insert property "buildDirectory"
    {
        Property p(m_engine->toScriptValue(project->buildDirectory));
        productProps->properties.insert("buildDirectory", p);
    }

    rproduct->fileTags = productProps->stringListValue("type");
    rproduct->destinationDirectory = productProps->stringValue("destination");
    foreach (const RulePtr &rule, globalRules)
        rproduct->rules.insert(rule);
    foreach (const FileTagger::ConstPtr &fileTagger, globalFileTaggers)
        rproduct->fileTaggers.insert(fileTagger);
    const QString lowerProductName = rproduct->name.toLower();
    products.insert(lowerProductName, rproduct);

    // resolve the modules for this product
    for (QHash<QString, Module::Ptr>::const_iterator modIt = data.product->modules.begin();
         modIt != data.product->modules.end(); ++modIt)
    {
        resolveModule(rproduct, modIt.key(), modIt.value()->object);
    }

    QList<EvaluationObject *> unresolvedChildren
            = resolveCommonItems(data.product->children, rproduct, dummyModule);

    // fill the product's configuration
    QVariantMap productCfg = evaluateAll(rproduct, data.product->scope);
    rproduct->properties->setValue(productCfg);

    // handle the 'Product.files' property
    {
        QScriptValue files = data.product->scope->property("files");
        if (files.isValid()) {
            resolveGroup(rproduct, data.product, data.product);
        }
    }

    foreach (EvaluationObject *child, unresolvedChildren) {
        const uint prototypeNameHash = qHash(child->prototype);
        if (prototypeNameHash == hashName_Group) {
            resolveGroup(rproduct, data.product, child);
        } else if (prototypeNameHash == hashName_ProductModule) {
            child->scope->properties.insert("product", Property(data.product));
            resolveProductModule(rproduct, child);
            data.usedProductsFromProductModule += child->unknownModules;
        }
    }

    // Apply file taggers.
    foreach (const SourceArtifactPtr &artifact, rproduct->allEnabledFiles())
        applyFileTaggers(artifact, rproduct);
    foreach (const ResolvedTransformer::Ptr &transformer, rproduct->transformers)
        foreach (const SourceArtifactPtr &artifact, transformer->outputs)
            applyFileTaggers(artifact, rproduct);

    // Merge duplicate artifacts.
    QHash<QString, QPair<GroupPtr, SourceArtifactPtr> > uniqueArtifacts;
    foreach (const GroupPtr &group, rproduct->groups) {
        Q_ASSERT(group->properties);

        QList<SourceArtifactPtr> artifactsInGroup = group->files;
        if (group->wildcards)
            artifactsInGroup += group->wildcards->files;

        foreach (const SourceArtifactPtr &artifact, artifactsInGroup) {
            QPair<GroupPtr, SourceArtifactPtr> p = uniqueArtifacts.value(artifact->absoluteFilePath);
            SourceArtifactPtr existing = p.second;
            if (existing) {
                // if an artifact is in the product and in a group, prefer the group configuration
                if (existing->properties != rproduct->properties)
                    throw Error(Tr::tr("Artifact '%1' is in more than one group.").arg(artifact->absoluteFilePath),
                                CodeLocation(m_project->fileName));

                // Remove the artifact that's in the product's group.
                p.first->files.removeOne(existing);

                // Merge artifacts.
                if (!artifact->overrideFileTags)
                    artifact->fileTags.unite(existing->fileTags);
            }
            uniqueArtifacts.insert(artifact->absoluteFilePath, qMakePair(group, artifact));
        }
    }

    project->products.append(rproduct);
}

void Loader::LoaderPrivate::resolveProductDependencies(const ResolvedProjectPtr &project,
        ProjectData &projectData, const ProductMap &resolvedProducts)
{
    // Collect product dependencies from ProductModules.
    bool productDependenciesAdded;
    do {
        productDependenciesAdded = false;
        foreach (ResolvedProductPtr rproduct, project->products) {
            ProductData &productData = projectData.products[rproduct];
            foreach (const UnknownModule::Ptr &unknownModule, productData.usedProducts) {
                const QString &usedProductName = unknownModule->name;
                QList<ResolvedProductPtr> usedProductCandidates = resolvedProducts.values(usedProductName);
                if (usedProductCandidates.count() < 1) {
                    if (!unknownModule->required) {
                        if (!unknownModule->failureMessage.isEmpty())
                            qbsWarning() << unknownModule->failureMessage;
                        continue;
                    }
                    throw Error(Tr::tr("Product dependency '%1' not found in '%2'.")
                                .arg(usedProductName, rproduct->location.fileName),
                                CodeLocation(m_project->fileName));
                }
                if (usedProductCandidates.count() > 1)
                    throw Error(Tr::tr("Product dependency '%1' is ambiguous.").arg(usedProductName),
                                CodeLocation(m_project->fileName));
                ResolvedProductPtr usedProduct = usedProductCandidates.first();
                const ProductData &usedProductData = projectData.products.value(usedProduct);
                bool added;
                productData.addUsedProducts(usedProductData.usedProductsFromProductModule, &added);
                if (added)
                    productDependenciesAdded = true;
            }
        }
    } while (productDependenciesAdded);

    // Resolve all inter-product dependencies.
    foreach (ResolvedProductPtr rproduct, project->products) {
        foreach (const UnknownModule::Ptr &unknownModule, projectData.products.value(rproduct).usedProducts) {
            const QString &usedProductName = unknownModule->name;
            QList<ResolvedProductPtr> usedProductCandidates = resolvedProducts.values(usedProductName);
            if (usedProductCandidates.count() < 1) {
                if (!unknownModule->required) {
                    if (!unknownModule->failureMessage.isEmpty())
                        qbsWarning() << unknownModule->failureMessage;
                    continue;
                }
                throw Error(Tr::tr("Product dependency '%1' not found.").arg(usedProductName),
                            CodeLocation(m_project->fileName));
            }
            if (usedProductCandidates.count() > 1)
                throw Error(Tr::tr("Product dependency '%1' is ambiguous.").arg(usedProductName),
                            CodeLocation(m_project->fileName));
            ResolvedProductPtr usedProduct = usedProductCandidates.first();
            rproduct->dependencies.insert(usedProduct);

            // insert the configuration of the ProductModule into the product's configuration
            const QVariantMap productModuleConfig = m_productModules.value(usedProductName);
            if (productModuleConfig.isEmpty())
                continue;

            QVariantMap productProperties = rproduct->properties->value();
            QVariantMap modules = productProperties.value("modules").toMap();
            modules.insert(usedProductName, productModuleConfig);
            productProperties.insert("modules", modules);
            rproduct->properties->setValue(productProperties);

            // insert the configuration of the ProductModule into the artifact configurations
            foreach (SourceArtifactPtr artifact, rproduct->allEnabledFiles()) {
                if (artifact->properties == rproduct->properties)
                    continue; // Already inserted above.
                QVariantMap sourceArtifactProperties = artifact->properties->value();
                QVariantMap modules = sourceArtifactProperties.value("modules").toMap();
                modules.insert(usedProductName, productModuleConfig);
                sourceArtifactProperties.insert("modules", modules);
                artifact->properties->setValue(sourceArtifactProperties);
            }
        }
    }
}

void Loader::LoaderPrivate::ProductData::addUsedProducts(const QList<UnknownModule::Ptr> &additionalUsedProducts, bool *productsAdded)
{
    int oldCount = usedProducts.count();
    QSet<QString> usedProductNames;
    foreach (const UnknownModule::Ptr &usedProduct, usedProducts)
        usedProductNames += usedProduct->name;
    foreach (const UnknownModule::Ptr &usedProduct, additionalUsedProducts)
        if (!usedProductNames.contains(usedProduct->name))
            usedProducts += usedProduct;
    *productsAdded = (oldCount != usedProducts.count());
}

} // namespace Internal
} // namespace qbs

QT_BEGIN_NAMESPACE
Q_DECLARE_TYPEINFO(qbs::ConditionalBinding, Q_MOVABLE_TYPE);
QT_END_NAMESPACE
