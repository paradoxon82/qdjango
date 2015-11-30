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

#ifndef QDJANGO_H
#define QDJANGO_H

#include "QDjangoMetaModel.h"

class QObject;
class QSqlDatabase;
class QSqlQuery;
class QString;

/** \brief The QDjango class provides a set of static functions.
 *
 *  It is used to access registered QDjangoModel classes.
 *
 * \ingroup Database
 */
class QDJANGO_DB_EXPORT QDjango
{
public:
    static bool createTables();
    static bool dropTables();

    static QSqlDatabase databaseForThread(QDjangoDatabase *database);
    static QSqlDatabase database(QByteArray modelName = QByteArray());
    static void setDatabase(QSqlDatabase database);
    static void setDatabaseForModel(QSqlDatabase database, QByteArray modelName);

    static bool isDebugEnabled();
    static void setDebugEnabled(bool enabled);

    template <class T>
    static QDjangoMetaModel registerModel(QSqlDatabase database = QSqlDatabase());

private:
    static QDjangoMetaModel registerModel(const QMetaObject *meta, QSqlDatabase database);
    static QDjangoMetaModel metaModel(const char *name);

    friend class QDjangoCompiler;
    friend class QDjangoModel;
    friend class QDjangoMetaModel;
    friend class QDjangoQuerySetPrivate;
};

/** Register a QDjangoModel class with QDjango.
 */
template <class T>
QDjangoMetaModel QDjango::registerModel(QSqlDatabase database)
{
    return registerModel(&T::staticMetaObject, database);
}

#endif
