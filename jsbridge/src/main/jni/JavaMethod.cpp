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
#include "JavaMethod.h"

#include "ExceptionHandler.h"
#include "JavaType.h"
#include "JniCache.h"
#include "JniTypes.h"
#include "JsBridgeContext.h"
#include "exceptions/JniException.h"
#include "log.h"
#include "jni-helpers/JniLocalRef.h"
#include "jni-helpers/JniLocalFrame.h"
#include "jni-helpers/JniContext.h"
#include "jni-helpers/JObjectArrayLocalRef.h"
#include <string>
#include <stdexcept>

JavaMethod::JavaMethod(const JsBridgeContext *jsBridgeContext, const JniLocalRef<jsBridgeMethod> &method, std::string methodName, bool isLambda)
 : m_methodName(std::move(methodName)),
   m_isLambda(isLambda) {

  const JniContext *jniContext = jsBridgeContext->getJniContext();
  MethodInterface methodInterface = jsBridgeContext->getJniCache()->getMethodInterface(method);

  m_isVarArgs = methodInterface.isVarArgs();
  JObjectArrayLocalRef parameters = methodInterface.getParameters();
  const jsize numParameters = parameters.getLength();

  m_argumentTypes.resize((size_t) numParameters);

  // Create JavaType instances
  for (jsize i = 0; i < numParameters; ++i) {
    JniLocalRef<jsBridgeParameter> parameter = parameters.getElement<jsBridgeParameter>(i);

    if (m_isVarArgs && i == numParameters - 1) {
      ParameterInterface parameterInterface = jsBridgeContext->getJniCache()->getParameterInterface(parameter);
      JniLocalRef<jsBridgeParameter> varArgParameter = parameterInterface.getGenericParameter();
      auto javaType = jsBridgeContext->getJavaTypeProvider().makeUniqueType(varArgParameter, m_isLambda /*boxed*/);
      m_argumentTypes[i] = std::move(javaType);
      break;
    }

    m_argumentTypes[i] = jsBridgeContext->getJavaTypeProvider().makeUniqueType(parameter, m_isLambda /*boxed*/);
  }

  parameters.release();

  {
    // Create return value loader
    JniLocalRef<jsBridgeParameter> returnParameter = methodInterface.getReturnParameter();
    m_returnValueType = jsBridgeContext->getJavaTypeProvider().makeUniqueType(returnParameter, m_isLambda /*boxed*/);
  }

  jmethodID methodId = nullptr;

  if (isLambda) {
    auto methodGlobal = JniGlobalRef<jsBridgeMethod>(method);

    m_methodBody = [=](const JniRef<jobject> &javaThis, const std::vector<JValue> &args) {
      JValue result = callLambda(jsBridgeContext, methodGlobal, javaThis, args);
#if defined(DUKTAPE)
      return m_returnValueType->push(result);
#elif defined(QUICKJS)
      return m_returnValueType->fromJava(result);
#endif
    };
  } else {
    JniLocalRef<jobject> javaMethod = methodInterface.getJavaMethod();
    methodId = jniContext->fromReflectedMethod(javaMethod);

    m_methodBody = [methodId, this](const JniRef<jobject> &javaThis, const std::vector<JValue> &args) {
      JValue result = m_returnValueType->callMethod(methodId, javaThis, args);
#if defined(DUKTAPE)
      return m_returnValueType->push(result);
#elif defined(QUICKJS)
      return m_returnValueType->fromJava(result);
#endif
    };
  }
}

#if defined(DUKTAPE)

#include "StackChecker.h"

duk_ret_t JavaMethod::invoke(const JsBridgeContext *jsBridgeContext, const JniRef<jobject> &javaThis) const {
  duk_context *ctx = jsBridgeContext->getDuktapeContext();

  const auto argCount = duk_get_top(ctx);
  const auto minArgs = m_isVarArgs
      ? m_argumentTypes.size() - 1
      : m_argumentTypes.size();

  if (argCount < minArgs) {
    // Not enough args
    alog_warn("Not enough parameters when calling Java method %s (expected: %d, received: %d). Missing parameters will be set to null.", m_methodName.c_str(), minArgs, argCount);
  }

  if (!m_isVarArgs && argCount > minArgs) {
    // Too many args
    throw std::invalid_argument(std::string() + "Too many parameters when calling Java method " + m_methodName + " (expected: " + std::to_string(minArgs) + ", received: " + std::to_string(argCount) + ")");
  }

  std::vector<JValue> args(m_argumentTypes.size());

  // Load the arguments off the stack and convert to Java types.
  // Note we're going backwards since the last argument is at the top of the stack.
  if (m_isVarArgs) {
    const auto &argumentType = m_argumentTypes.back();
    args[args.size() - 1] = argumentType->popArray(argCount - minArgs, true /*expanded*/);
  }
  for (ssize_t i = minArgs - 1; i >= 0; --i) {
    const auto &argumentType = m_argumentTypes[i];
    JValue value;
    if (i >= argCount) {
      // Parameter not given by JS: set it to null
      // Note: we do not explicitly check if the parameter is nullable so the execution might throw!
    } else {
      value = argumentType->pop();
    }
    args[i] = std::move(value);
  }

  return m_methodBody(javaThis, args);
}

#elif defined(QUICKJS)

JSValue JavaMethod::invoke(const JsBridgeContext *jsBridgeContext, const JniRef<jobject> &javaThis, int argc, JSValueConst *argv) const {

  JSContext *ctx = jsBridgeContext->getQuickJsContext();
  const JniContext *jniContext = jsBridgeContext->getJniContext();

  const int minArgs = m_isVarArgs
      ? m_argumentTypes.size() - 1
      : m_argumentTypes.size();

  if (argc < minArgs) {
    // Not enough args
    alog_warn("Not enough parameters when calling Java method %s (expected: %d, received: %d). Missing parameters will be set to null.", m_methodName.c_str(), minArgs, argc);
  }

  if (!m_isVarArgs && argc > minArgs) {
    // Too many args
    throw std::invalid_argument(std::string() + "Too many parameters when calling Java method " + m_methodName + " (expected: " + std::to_string(minArgs) + ", received: " + std::to_string(argc) + ")");
  }

  std::vector<JValue> args(m_argumentTypes.size());

  // Load arguments and convert to Java types
  for (int i = 0; i < minArgs; ++i) {
    const auto &argumentType = m_argumentTypes[i];
    JValue value;
    if (i >= argc) {
      // Parameter not given by JS: set it to null
      // Note: we do not explicitly check if the parameter is nullable so the execution might throw!
    } else {
      value = argumentType->toJava(argv[i]);
    }
    args[i] = std::move(value);
  }

  if (m_isVarArgs) {
    // Move the varargs into a JS array before converting it to a Java array
    const auto &argumentType = m_argumentTypes.back();
    int varArgCount = argc - minArgs;
    JSValue varArgArray = JS_NewArray(ctx);
    for (int i = 0; i < varArgCount; ++i) {
      JS_SetPropertyUint32(ctx, varArgArray, static_cast<uint32_t>(i), JS_DupValue(ctx, argv[minArgs + i]));
    }
    args[args.size() - 1] = argumentType->toJavaArray(varArgArray);
    JS_FreeValue(ctx, varArgArray);
  }


  return m_methodBody(javaThis, args);
}

#endif

// static
JValue JavaMethod::callLambda(const JsBridgeContext *jsBridgeContext, const JniRef<jsBridgeMethod> &method, const JniRef<jobject> &javaThis, const std::vector<JValue> &args) {
  const JniContext *jniContext = jsBridgeContext->getJniContext();
  assert(jniContext != nullptr);

  const JniCache *jniCache = jsBridgeContext->getJniCache();

  JniLocalRef<jclass> objectClass = jniCache->getObjectClass();
  JObjectArrayLocalRef argArray(jniContext, args.size(), objectClass);
  int i = 0;
  for (const auto &arg : args) {
    const auto &argLocalRef = arg.getLocalRef();
    argArray.setElement(i++, argLocalRef);
  }

  JniLocalRef<jobject> ret = jniCache->getMethodInterface(method).callNativeLambda(javaThis, argArray);

  if (jniContext->exceptionCheck()) {
    throw JniException(jniContext);
  }

  return JValue(ret);
}


