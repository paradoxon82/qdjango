/*
 * Copyright (C) 2010-2015 Jeremy Lainé
 * Contact: https://github.com/jlaine/qdjango
 *
 * This file is part of the QDjango Library.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QThread>
#include <QStack>

#include "QDjango.h"

static const char *defaultConnectionPrefix = "_qdjango_";

QMap<QByteArray, QDjangoMetaModel> globalMetaModels = QMap<QByteArray, QDjangoMetaModel>();
QMap<QByteArray, QDjangoDatabase*> globalModelDbMap = QMap<QByteArray, QDjangoDatabase*>();
static QDjangoDatabase *globalDatabase = 0;
// contains the types of all database connections registered wich QDjango::initDatabase(), also thread specific DBs
QMap<QString, QDjangoDatabase::DatabaseType> globalDatabaseTypeMap = QMap<QString, QDjangoDatabase::DatabaseType>();
// contains the QDjangoDatabase object associated with the main connections associated with models
// the globalDatabase and all the thread specific connections are missing
QMap<QString, QDjangoDatabase*> globalConnectionDbMap = QMap<QString, QDjangoDatabase*>();
static bool globalDebugEnabled = false;

/// \cond

QDjangoDatabase::QDjangoDatabase(QObject *parent)
    : QObject(parent), connectionId(0)
{
}

void QDjangoDatabase::threadFinished()
{
    QThread *thread = qobject_cast<QThread*>(sender());
    if (!thread)
        return;

    // cleanup database connection for the thread
    QMutexLocker locker(&mutex);
    disconnect(thread, SIGNAL(finished()), this, SLOT(threadFinished()));
    const QString connectionName = copies.value(thread).connectionName();
    copies.remove(thread);
    const QString connectionPrefix = getConnectionPrefix();
    if (connectionName.startsWith(connectionPrefix))
        QSqlDatabase::removeDatabase(connectionName);
}

static void closeDatabase()
{
    delete globalDatabase;
    qDeleteAll(globalModelDbMap);
}

static QDjangoDatabase::DatabaseType getDatabaseType(QSqlDatabase &db)
{
    const QString driverName = db.driverName();
    if (driverName == QLatin1String("QMYSQL") ||
        driverName == QLatin1String("QMYSQL3"))
        return QDjangoDatabase::MySqlServer;
    else if (driverName == QLatin1String("QSQLITE") ||
             driverName == QLatin1String("QSQLITE2"))
        return QDjangoDatabase::SQLite;
    else if (driverName == QLatin1String("QPSQL"))
        return QDjangoDatabase::PostgreSQL;
    else if (driverName == QLatin1String("QODBC")) {
        QSqlQuery query(db);
        if (query.exec("SELECT sqlite_version()"))
            return QDjangoDatabase::SQLite;

        if (query.exec("SELECT @@version") && query.next() &&
            query.value(0).toString().contains("Microsoft SQL Server"))
                return QDjangoDatabase::MSSqlServer;

        if (query.exec("SELECT version()") && query.next()) {
            if (query.value(0).toString().contains("PostgreSQL"))
                return QDjangoDatabase::PostgreSQL;
            else
                return QDjangoDatabase::MySqlServer;
        }
    }
    return QDjangoDatabase::UnknownDB;
}

static void initDatabase(QSqlDatabase db)
{
    QDjangoDatabase::DatabaseType databaseType = getDatabaseType(db);
    if (databaseType == QDjangoDatabase::SQLite) {
        // enable foreign key constraint handling
        QDjangoQuery query(db);
        query.prepare("PRAGMA foreign_keys=on");
        query.exec();
    }
    globalDatabaseTypeMap.insert(db.connectionName(), databaseType);
}

QDjangoQuery::QDjangoQuery(QSqlDatabase db)
    : QSqlQuery(db)
{
    if (QDjangoDatabase::databaseType(db) == QDjangoDatabase::MSSqlServer) {
        // default to fast-forward cursor
        setForwardOnly(true);
    }
}

void QDjangoQuery::addBindValue(const QVariant &val, QSql::ParamType paramType)
{
    // this hack is required so that we do not store a mix of local
    // and UTC times
    if (val.type() == QVariant::DateTime)
        QSqlQuery::addBindValue(val.toDateTime().toLocalTime(), paramType);
    else
        QSqlQuery::addBindValue(val, paramType);
}

bool QDjangoQuery::exec()
{
    if (globalDebugEnabled) {
        qDebug() << "SQL query" << lastQuery();
        QMapIterator<QString, QVariant> i(boundValues());
        while (i.hasNext()) {
            i.next();
            qDebug() << "SQL   " << i.key().toLatin1().data() << "="
                     << i.value().toString().toLatin1().data();
        }
    }
    if (!QSqlQuery::exec()) {
        if (globalDebugEnabled)
            qWarning() << "SQL error" << lastError();
        return false;
    }
    return true;
}

bool QDjangoQuery::exec(const QString &query)
{
    if (globalDebugEnabled)
        qDebug() << "SQL query" << query;
    if (!QSqlQuery::exec(query)) {
        if (globalDebugEnabled)
            qWarning() << "SQL error" << lastError();
        return false;
    }
    return true;
}

/// \endcond

QString QDjangoDatabase::getConnectionPrefix()
{
  QString prefix = reference.connectionName();
  if (prefix.isEmpty())
    { // the database is the default database, use the default prefix
      prefix = QLatin1String(defaultConnectionPrefix);
    }
  return prefix;
}

QSqlDatabase QDjango::databaseForThread(QDjangoDatabase *database)
{
  // if we are in the main thread, return reference connection
  QThread *thread = QThread::currentThread();
  if (thread == database->thread())
      return database->reference;

  // if we have a connection for this thread, return it
  QMutexLocker locker(&database->mutex);
  if (database->copies.contains(thread))
      return database->copies[thread];

  // create a new connection for this thread
  QObject::connect(thread, SIGNAL(finished()), database, SLOT(threadFinished()));
  QString connectionPrefix = database->getConnectionPrefix();
  QSqlDatabase db = QSqlDatabase::cloneDatabase(database->reference,
      connectionPrefix + QString::number(database->connectionId++));
  db.open();
  initDatabase(db);
  database->copies.insert(thread, db);
  return db;
}

/*!
    Returns the database used by QDjango.

    If you call this method from any thread but the application's main thread,
    a new connection to the database will be created. The connection will
    automatically be torn down once the thread finishes.

    \sa setDatabase()
*/
QSqlDatabase QDjango::database(QByteArray modelName)
{
  QDjangoDatabase* database;
  if (globalModelDbMap.contains(modelName))
    {
      database = globalModelDbMap[modelName];
    }
  else
    {
      database = globalDatabase;
    }

  if (!database)
{
    if (!globalDatabase)
        return QSqlDatabase();
    }

  return databaseForThread(database);
}

/*!
    Sets the database used by QDjango.

    You must call this method from your application's main thread.

    \sa database()
*/
void QDjango::setDatabase(QSqlDatabase database)
{
    QDjangoDatabase::DatabaseType databaseType = getDatabaseType(database);
    if (databaseType == QDjangoDatabase::UnknownDB) {
        qWarning() << "Unsupported database driver" << database.driverName();
    }

    if (!globalDatabase)
    {
        globalDatabase = new QDjangoDatabase();
        qAddPostRoutine(closeDatabase);
    }
    initDatabase(database);
    globalDatabase->reference = database;
}

void QDjango::setDatabaseForModel(QSqlDatabase databaseConn, QByteArray modelName)
{
  QDjangoDatabase::DatabaseType databaseType = getDatabaseType(databaseConn);
  if (databaseType == QDjangoDatabase::UnknownDB) {
      qWarning() << "Unsupported database driver" << databaseConn.driverName();
  }

  if (globalModelDbMap.contains(modelName))
    {
      // model was already assigned to a db
      if (globalModelDbMap[modelName]->reference.connectionName() != databaseConn.connectionName())
        {
          // but it seems to be a different connection, so remove it assign a new one
          globalModelDbMap.remove(modelName);
        }
    }

  if (!globalModelDbMap.contains(modelName))
    {
      // the QDjangoDatabase is not yet known for this model
      const QString connectionName = databaseConn.connectionName();
      if (!globalConnectionDbMap.contains(connectionName))
        {
          // there is no QDjangoDatabase object yet assigned to the connection
          QDjangoDatabase *database = new QDjangoDatabase();
          qAddPostRoutine(closeDatabase);
          initDatabase(databaseConn);
          database->reference = databaseConn;
          globalConnectionDbMap[connectionName] = database;
        }
      globalModelDbMap[modelName] = globalConnectionDbMap[connectionName];
    }
  else
    {
      // TODO close DB or warn?
    }
}

/*!
    Returns whether debugging information should be printed.

    \sa setDebugEnabled()
*/
bool QDjango::isDebugEnabled()
{
    return globalDebugEnabled;
}

/*!
    Sets whether debugging information should be printed.

    \sa isDebugEnabled()
*/
void QDjango::setDebugEnabled(bool enabled)
{
    globalDebugEnabled = enabled;
}

static void qdjango_topsort(const QByteArray &modelName, QHash<QByteArray, bool> &visited,
                            QStack<QDjangoMetaModel> &stack)
{
    visited[modelName] = true;
    QDjangoMetaModel model = globalMetaModels[modelName];
    foreach (const QByteArray &foreignModel, model.foreignFields().values()) {
        if (!visited[foreignModel])
            qdjango_topsort(foreignModel, visited, stack);
    }

    stack.push(model);
}

static QStack<QDjangoMetaModel> qdjango_sorted_metamodels()
{
    QStack<QDjangoMetaModel> stack;
    stack.reserve(globalMetaModels.size());
    QHash<QByteArray, bool> visited;
    visited.reserve(globalMetaModels.size());
    foreach (const QByteArray &model, globalMetaModels.keys())
        visited[model] = false;

    foreach (const QByteArray &model, globalMetaModels.keys()) {
        if (!visited[model])
            qdjango_topsort(model, visited, stack);
    }

    return stack;
}

/*!
    Creates the database tables for all registered models.

    \return true if all the tables were created, false otherwise.
*/
bool QDjango::createTables()
{
    bool result = true;
    QStack<QDjangoMetaModel> stack = qdjango_sorted_metamodels();
    foreach (const QDjangoMetaModel &model, stack) {
        if (!model.createTable())
            result = false;
    }

    return result;
}

/*!
    Drops the database tables for all registered models.

    \return true if all the tables were dropped, false otherwise.
*/
bool QDjango::dropTables()
{
    bool result = true;
    QStack<QDjangoMetaModel> stack = qdjango_sorted_metamodels();
    for (int i = stack.size() - 1; i >= 0; --i) {
        QDjangoMetaModel model = stack.at(i);
        if (!model.dropTable())
            result = false;
    }

    return result;
}

/*!
    Returns the QDjangoMetaModel with the given \a name.
 */
QDjangoMetaModel QDjango::metaModel(const char *name)
{
    if (globalMetaModels.contains(name))
        return globalMetaModels.value(name);

    // otherwise, try to find a model anyway
    foreach (QByteArray modelName, globalMetaModels.keys()) {
        if (qstricmp(name, modelName.data()) == 0)
            return globalMetaModels.value(modelName);
    }

    return QDjangoMetaModel();
}

QDjangoMetaModel QDjango::registerModel(const QMetaObject *meta, QSqlDatabase database)
{
    const QByteArray name = meta->className();
    if (!globalMetaModels.contains(name))
      {
        globalMetaModels.insert(name, QDjangoMetaModel(meta));
      }
    if (database.isValid())
      {
        setDatabaseForModel(database, name);
      }

    return globalMetaModels[name];
}

QDjangoDatabase::DatabaseType QDjangoDatabase::databaseType(const QSqlDatabase &db)
{
//    Q_UNUSED(db);
//    return globalDatabaseType;
  return globalDatabaseTypeMap.value(db.connectionName(), QDjangoDatabase::UnknownDB);
}
