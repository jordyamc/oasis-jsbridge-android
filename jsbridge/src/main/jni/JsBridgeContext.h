/*
 * Copyright (C) 2019 ProSiebenSat1.Digital GmbH.
 *
 * Originally based on Duktape Android:
 * Copyright (C) 2015 Square, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef _JSBRIDGE_JSBRIDGECONTEXT_H
#define _JSBRIDGE_JSBRIDGECONTEXT_H

#include "JavaTypeProvider.h"
#include "jni-helpers/JniLocalRef.h"
#include "jni-helpers/JniContext.h"
#include "jni-helpers/JObjectArrayLocalRef.h"
#include <jni.h>
#include <string>

#if defined(DUKTAPE)
# include "duktape/duktape.h"
#else
# include "quickjs/quickjs.h"
#endif

class DuktapeUtils;
class ExceptionHandler;
class JavaType;
class JniCache;
class JObjectArrayLocalRef;
class QuickJsUtils;

// JS context, delegating operations to the JS engine.
class JsBridgeContext {

public:
  JsBridgeContext();
  JsBridgeContext(const JsBridgeContext &) = delete;
  JsBridgeContext & operator=(const JsBridgeContext &) = delete;

  ~JsBridgeContext();

  // Must be called immediately after the constructor
  void init(JniContext *jniContext, const JniLocalRef<jobject> &jsBridgeObject);

  void startDebugger(int port);
  void cancelDebug();

  JValue evaluateString(const JStringLocalRef &strSourceCode, const JniLocalRef<jsBridgeParameter> &returnParameter,
                        bool awaitJsPromise) const;
  void evaluateFileContent(const JStringLocalRef &strSourceCode, const std::string &strFileName, bool asModule) const;

  void registerJavaObject(const std::string &strName, const JniLocalRef<jobject> &object,
                                  const JObjectArrayLocalRef &methods);
  void registerJavaLambda(const std::string &strName, const JniLocalRef<jobject> &object,
                                  const JniLocalRef<jsBridgeMethod> &method);
  void registerJsObject(const std::string &strName, const JObjectArrayLocalRef &methods, bool check);
  void registerJsLambda(const std::string &strName, const JniLocalRef<jsBridgeMethod> &method);
  JValue callJsMethod(const std::string &objectName, const JniLocalRef<jobject> &javaMethod,
                              const JObjectArrayLocalRef &args, bool awaitJsPromise);
  JValue callJsLambda(const std::string &strFunctionName, const JObjectArrayLocalRef &args,
                              bool awaitJsPromise);

  void assignJsValue(const std::string &strGlobalName, const JStringLocalRef &strCode);
  void deleteJsValue(const std::string &strGlobalName);
  void copyJsValue(const std::string &strGlobalNameTo, const std::string &strGlobalNameFrom);
  void newJsFunction(const std::string &strGlobalName, const JObjectArrayLocalRef &args, const JStringLocalRef &strCode);

  void convertJavaValueToJs(const std::string &strGlobalName, const JniLocalRef<jobject> &javaValue, const JniLocalRef<jsBridgeParameter> &parameter);

  void processPromiseQueue();

  JniContext *getJniContext() { return m_jniContext; }
  const JniContext *getJniContext() const { return m_jniContext; }
  const JniCache *getJniCache() const { return m_jniCache; }
  const ExceptionHandler *getExceptionHandler() const { return m_exceptionHandler; }

  const JavaTypeProvider &getJavaTypeProvider() const { return m_javaTypeProvider; }

#if defined(DUKTAPE)
  static JsBridgeContext *getInstance(duk_context *);

  DuktapeUtils *getUtils() const { return m_utils; }
  duk_context *getDuktapeContext() const { return m_ctx; };
#elif defined(QUICKJS)
  static JsBridgeContext *getInstance(JSContext *);

  QuickJsUtils *getUtils() const { return m_utils; }
  JSContext *getQuickJsContext() const { return m_ctx; };
#endif

private:
  // Updated on each Java -> Native call (and reset to nullptr afterwards)
  JniContext *m_jniContext = nullptr;
  JniCache *m_jniCache = nullptr;
  ExceptionHandler *m_exceptionHandler = nullptr;

  const JavaTypeProvider m_javaTypeProvider;

#if defined(DUKTAPE)
  duk_context *m_ctx = nullptr;
  DuktapeUtils *m_utils = nullptr;
#elif defined(QUICKJS)
  JSRuntime *m_runtime = nullptr;
  JSContext *m_ctx = nullptr;
  QuickJsUtils *m_utils = nullptr;
#endif
};

#endif
