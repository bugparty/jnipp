#ifdef _WIN32
# define WIN32_LEAN_AND_MEAN 1

  // Windows Dependencies
# include <windows.h>
#endif

// External Dependencies
#include <jni.h>

// Standard Dependencies
#include <atomic>
#include <string>

// Local Dependencies
#include "jnipp.h"

namespace jni
{
	// Static Variables
	static std::atomic_bool isVm(false);
	static JavaVM* javaVm = nullptr;

	/**
		Maintains the lifecycle of a JNIEnv.
	 */
	class ScopedEnv final
	{
	public:
		ScopedEnv() : _vm(nullptr), _env(nullptr), _attached(false) {}
		~ScopedEnv();

		void init(JavaVM* vm);
		JNIEnv* get() const { return _env; }

	private:
		// Instance Variables
		JavaVM* _vm;
		JNIEnv* _env;
		bool    _attached;	///< Manually attached, as opposed to already attached.
	};

	ScopedEnv::~ScopedEnv()
	{
		if (_vm && _attached)
			_vm->DetachCurrentThread();
	}

	void ScopedEnv::init(JavaVM* vm)
	{
		if (_env != nullptr)
			return;

		if (vm == nullptr)
			throw InitializationException("JNI not initialized");

		if (vm->GetEnv((void**) &_env, JNI_VERSION_1_2) != JNI_OK)
		{
#ifdef __ANDROID__
			if (vm->AttachCurrentThread(&_env, nullptr) != 0)
#else
			if (vm->AttachCurrentThread((void**) &_env, nullptr) != 0)
#endif
				throw InitializationException("Could not attach JNI to thread");

			_attached = true;
		}
	}

	/*
		Helper Functions
	 */

#ifndef _WIN32

	std::wstring toWString(const jchar* source, jsize length)
	{
		std::wstring result;

		result.reserve(length * 2);

		for (size_t i = 0; i < length; ++i)
		{
			wchar_t ch = source[i];

			// Check for a two-segment character.
			if (ch >= wchar_t(0xD800) && ch <= wchar_t(0xDBFF)) {
				if (i + 1 >= length)
					break;

				// Create a single, 32-bit character.
				ch = (ch - wchar_t(0xD800)) << 10;
				ch += source[i++] - wchar_t(0xDC00 + 0x10000);
			}

			result += ch;
		}

		return result;
	}

#endif // _WIN32

	JNIEnv* env()
	{
		static thread_local ScopedEnv env;

		if (env.get() == nullptr)
			env.init(javaVm);

		return env.get();
	}

	static jclass findClass(const char* name)
	{
		jclass ref = env()->FindClass(name);

		if (ref == NULL)
			throw NameResolutionException(name);

		return ref;
	}

	static void handleJavaExceptions()
	{
		JNIEnv* env = jni::env();

		jthrowable exception = env->ExceptionOccurred();

		if (exception != NULL)
		{
			Object obj(exception, Object::Temporary);

			env->ExceptionClear();

			throw InvocationException(obj.call<std::string>("toString").c_str());
		}
	}

	static std::string toString(jobject handle, bool deleteLocal = true)
	{
		std::string result;

		if (handle != NULL)
		{
			JNIEnv* env = jni::env();

			const char* chars = env->GetStringUTFChars(jstring(handle), NULL);
			result.assign(chars, env->GetStringUTFLength(jstring(handle)));
			env->ReleaseStringUTFChars(jstring(handle), chars);

			if (deleteLocal)
				env->DeleteLocalRef(handle);
		}

		return result;
	}

	static std::wstring toWString(jobject handle, bool deleteLocal = true)
	{
		std::wstring result;

		if (handle != NULL)
		{
			JNIEnv* env = jni::env();

			const jchar* chars = env->GetStringChars(jstring(handle), NULL);
#ifdef _WIN32
			result.assign((const wchar_t*) chars, env->GetStringLength(jstring(handle)));
#else
			result = toWString(chars, env->GetStringLength(jstring(handle)));
#endif
			env->ReleaseStringChars(jstring(handle), chars);

			if (deleteLocal)
				env->DeleteLocalRef(handle);
		}

		return result;
	}


	/*
		Stand-alone Function Impelementations
	 */

	void init(JNIEnv* env)
	{
		bool expected = false;

		if (isVm.compare_exchange_strong(expected, true))
		{
			if (javaVm == nullptr && env->GetJavaVM(&javaVm) != 0)
				throw InitializationException("Could not acquire Java VM");
		}
	}

	/*
		Object Implementation
	 */

	Object::Object() noexcept : _handle(NULL), _class(NULL), _isGlobal(false) 
	{
	}

	Object::Object(const Object& other) : _handle(NULL), _class(NULL), _isGlobal(!other.isNull())
	{
		if (!other.isNull())
			_handle = env()->NewGlobalRef(other._handle);
	}

	Object::Object(Object&& other) noexcept : _handle(other._handle), _class(other._class), _isGlobal(other._isGlobal)
	{
		other._handle = NULL;
		other._class  = NULL;
		other._isGlobal = false;
	}

	Object::Object(jobject ref, int scopeFlags) : _handle(ref), _class(NULL), _isGlobal((scopeFlags & Temporary) == 0)
	{
		if (!_isGlobal)
			return;

		JNIEnv* env = jni::env();

		_handle = env->NewGlobalRef(ref);

		if (scopeFlags & DeleteLocalInput)
			env->DeleteLocalRef(ref);
	}

	Object::~Object() noexcept
	{
		JNIEnv* env = jni::env();

		if (_isGlobal)
			env->DeleteGlobalRef(_handle);

		if (_class != NULL)
			env->DeleteGlobalRef(_class);
	}

	Object& Object::operator=(const Object& other)
	{
		if (_handle != other._handle)
		{
			JNIEnv* env = jni::env();

			// Ditch the old reference.
			if (_isGlobal)
				env->DeleteGlobalRef(_handle);
			if (_class != NULL)
				env->DeleteGlobalRef(_class);

			// Assign the new reference.
			if ((_isGlobal = !other.isNull()) != false)
				_handle = env->NewGlobalRef(other._handle);

			_class = NULL;
		}

		return *this;
	}

	bool Object::operator==(const Object& other) const
	{
		return env()->IsSameObject(_handle, other._handle) != JNI_FALSE;
	}

	Object& Object::operator=(Object&& other)
	{
		if (_handle != other._handle)
		{
			JNIEnv* env = jni::env();

			// Ditch the old reference.
			if (_isGlobal)
				env->DeleteGlobalRef(_handle);
			if (_class != NULL)
				env->DeleteGlobalRef(_class);

			// Assign the new reference.
			_handle   = other._handle;
			_isGlobal = other._isGlobal;
			_class    = other._class;

			other._handle   = NULL;
			other._isGlobal = false;
			other._class    = NULL;
		}

		return *this;
	}

	bool Object::isNull() const noexcept
	{
		return _handle == NULL || env()->IsSameObject(_handle, NULL);
	}

	template <>	bool Object::callMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallBooleanMethodA(_handle, method, (jvalue*) args);
		handleJavaExceptions();
		return result != 0;
	}

	template <> bool Object::get(field_t field) const
	{
		return env()->GetBooleanField(_handle, field) != 0;
	}

	template <> void Object::set(field_t field, const bool& value)
	{
		env()->SetBooleanField(_handle, field, value);
	}

	template <>	wchar_t Object::callMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallCharMethodA(_handle, method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <> wchar_t Object::get(field_t field) const
	{
		return env()->GetCharField(_handle, field);
	}

	template <> void Object::set(field_t field, const wchar_t& value)
	{
		env()->SetCharField(_handle, field, value);
	}

	template <>	short Object::callMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallShortMethodA(_handle, method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <> short Object::get(field_t field) const
	{
		return env()->GetShortField(_handle, field);
	}

	template <> void Object::set(field_t field, const short& value)
	{
		env()->SetShortField(_handle, field, value);
	}

	template <>	int Object::callMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallIntMethodA(_handle, method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <> int Object::get(field_t field) const
	{
		return env()->GetIntField(_handle, field);
	}

	template <> void Object::set(field_t field, const int& value)
	{
		env()->SetIntField(_handle, field, value);
	}

	template <>	long long Object::callMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallLongMethodA(_handle, method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <> long long Object::get(field_t field) const
	{
		return env()->GetLongField(_handle, field);
	}

	template <> void Object::set(field_t field, const long long& value)
	{
		env()->SetLongField(_handle, field, value);
	}

	template <>	float Object::callMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallFloatMethodA(_handle, method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <> float Object::get(field_t field) const
	{
		return env()->GetFloatField(_handle, field);
	}

	template <> void Object::set(field_t field, const float& value)
	{
		env()->SetFloatField(_handle, field, value);
	}

	template <>	double Object::callMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallDoubleMethodA(_handle, method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <> double Object::get(field_t field) const
	{
		return env()->GetDoubleField(_handle, field);
	}

	template <> void Object::set(field_t field, const double& value)
	{
		env()->SetDoubleField(_handle, field, value);
	}

	template <>	std::string Object::callMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallObjectMethodA(_handle, method, (jvalue*) args);
		handleJavaExceptions();
		return toString(result);
	}

	template <>	std::wstring Object::callMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallObjectMethodA(_handle, method, (jvalue*) args);
		handleJavaExceptions();
		return toWString(result);
	}

	template <> std::string Object::get(field_t field) const
	{
		return toString(env()->GetObjectField(_handle, field));
	}

	template <> std::wstring Object::get(field_t field) const
	{
		return toWString(env()->GetObjectField(_handle, field));
	}


	template <> void Object::set(field_t field, const std::string& value)
	{
		JNIEnv* env = jni::env();

		jobject handle = env->NewStringUTF(value.c_str());
		env->SetObjectField(_handle, field, handle);
		env->DeleteLocalRef(handle);
	}

#ifdef _WIN32

	template <> void Object::set(field_t field, const std::wstring& value)
	{
		JNIEnv* env = jni::env();

		jobject handle = env->NewString((const jchar*) value.c_str(), jsize(value.length()));
		env->SetObjectField(_handle, field, handle);
		env->DeleteLocalRef(handle);
	}

	template <> void Object::set(field_t field, const wchar_t* const& value)
	{
		JNIEnv* env = jni::env();

		jobject handle = env->NewString((const jchar*) value, jsize(std::wcslen(value)));
		env->SetObjectField(_handle, field, handle);
		env->DeleteLocalRef(handle);
	}

#else
# error "utf32"
#endif

	template <> void Object::set(field_t field, const char* const& value)
	{
		JNIEnv* env = jni::env();

		jobject handle = env->NewStringUTF(value);
		env->SetObjectField(_handle, field, handle);
		env->DeleteLocalRef(handle);
	}

	jclass Object::getClass() const
	{
		if (_class == NULL)
		{
			JNIEnv* env = jni::env();

			jclass classRef = env->GetObjectClass(_handle);
			_class = jclass(env->NewGlobalRef(classRef));
			env->DeleteLocalRef(classRef);
		}

		return _class;
	}

	method_t Object::getMethod(const char* name, const char* signature) const
	{
		return Class(getClass(), Temporary).getMethod(name, signature);
	}

	field_t Object::getField(const char* name, const char* signature) const
	{
		return Class(getClass(), Temporary).getField(name, signature);
	}

	/*
		Class Implementation
	 */

	Class::Class(const char* name) : Object(findClass(name), DeleteLocalInput)
	{
	}

	Class::Class(jclass ref, int scopeFlags) : Object(ref, scopeFlags)
	{
	}

	Object Class::newInstance() const
	{
		method_t constructor = getMethod("<init>", "()V");
		jobject obj = env()->NewObject(jclass(getHandle()), constructor);

		handleJavaExceptions();

		return Object(obj, Object::DeleteLocalInput);
	}

	field_t Class::getField(const char* name, const char* signature) const
	{
		jfieldID id = env()->GetFieldID(jclass(getHandle()), name, signature);

		if (id == nullptr)
			throw NameResolutionException(name);

		return id;
	}

	field_t Class::getStaticField(const char* name, const char* signature) const
	{
		jfieldID id = env()->GetStaticFieldID(jclass(getHandle()), name, signature);

		if (id == nullptr)
			throw NameResolutionException(name);

		return id;
	}

	method_t Class::getMethod(const char* name, const char* signature) const
	{
		jmethodID id = env()->GetMethodID(jclass(getHandle()), name, signature);

		if (id == nullptr)
			throw NameResolutionException(name);

		return id;
	}

	method_t Class::getStaticMethod(const char* name, const char* signature) const
	{
		jmethodID id = env()->GetStaticMethodID(jclass(getHandle()), name, signature);

		if (id == nullptr)
			throw NameResolutionException(name);

		return id;
	}

	Class Class::getParent() const
	{
		return Class(env()->GetSuperclass(jclass(getHandle())), DeleteLocalInput);
	}

	std::string Class::getName() const
	{
		return Object::call<std::string>("getName");
	}

	template <>	bool Class::callStaticMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallStaticBooleanMethodA(jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result != 0;
	}

	template <>	bool Class::callExactMethod(jobject obj, method_t method, internal::value_t* args) const
	{
		auto result = env()->CallNonvirtualBooleanMethodA(obj, jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result != 0;
	}

	template <> bool Class::get(field_t field) const
	{
		return env()->GetStaticBooleanField(jclass(getHandle()), field) != 0;
	}

	template <> void Class::set(field_t field, const bool& value)
	{
		env()->SetStaticBooleanField(jclass(getHandle()), field, value);
	}

	template <>	wchar_t Class::callStaticMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallStaticCharMethodA(jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <>	wchar_t Class::callExactMethod(jobject obj, method_t method, internal::value_t* args) const
	{
		auto result = env()->CallNonvirtualCharMethodA(obj, jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <> wchar_t Class::get(field_t field) const
	{
		return env()->GetStaticCharField(jclass(getHandle()), field);
	}

	template <> void Class::set(field_t field, const wchar_t& value)
	{
		env()->SetStaticCharField(jclass(getHandle()), field, value);
	}

	template <>	short Class::callStaticMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallStaticShortMethodA(jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <>	short Class::callExactMethod(jobject obj, method_t method, internal::value_t* args) const
	{
		auto result = env()->CallNonvirtualShortMethodA(obj, jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <> short Class::get(field_t field) const
	{
		return env()->GetStaticShortField(jclass(getHandle()), field);
	}

	template <> void Class::set(field_t field, const short& value)
	{
		env()->SetStaticShortField(jclass(getHandle()), field, value);
	}

	template <>	int Class::callStaticMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallStaticIntMethodA(jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <>	int Class::callExactMethod(jobject obj, method_t method, internal::value_t* args) const
	{
		auto result = env()->CallNonvirtualIntMethodA(obj, jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <> int Class::get(field_t field) const
	{
		return env()->GetStaticIntField(jclass(getHandle()), field);
	}

	template <> void Class::set(field_t field, const int& value)
	{
		env()->SetStaticIntField(jclass(getHandle()), field, value);
	}

	template <>	long long Class::callStaticMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallStaticLongMethodA(jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <>	long long Class::callExactMethod(jobject obj, method_t method, internal::value_t* args) const
	{
		auto result = env()->CallNonvirtualLongMethodA(obj, jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <> long long Class::get(field_t field) const
	{
		return env()->GetStaticLongField(jclass(getHandle()), field);
	}

	template <> void Class::set(field_t field, const long long& value)
	{
		env()->SetStaticLongField(jclass(getHandle()), field, value);
	}

	template <>	float Class::callStaticMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallStaticFloatMethodA(jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <>	float Class::callExactMethod(jobject obj, method_t method, internal::value_t* args) const
	{
		auto result = env()->CallNonvirtualFloatMethodA(obj, jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <> float Class::get(field_t field) const
	{
		return env()->GetStaticFloatField(jclass(getHandle()), field);
	}

	template <> void Class::set(field_t field, const float& value)
	{
		env()->SetStaticFloatField(jclass(getHandle()), field, value);
	}

	template <>	double Class::callStaticMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallStaticDoubleMethodA(jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <>	double Class::callExactMethod(jobject obj, method_t method, internal::value_t* args) const
	{
		auto result = env()->CallNonvirtualDoubleMethodA(obj, jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return result;
	}

	template <> double Class::get(field_t field) const
	{
		return env()->GetStaticDoubleField(jclass(getHandle()), field);
	}

	template <> void Class::set(field_t field, const double& value)
	{
		env()->SetStaticDoubleField(jclass(getHandle()), field, value);
	}

	template <>	std::string Class::callStaticMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallStaticObjectMethodA(jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return toString(result);
	}

	template <>	std::string Class::callExactMethod(jobject obj, method_t method, internal::value_t* args) const
	{
		auto result = env()->CallNonvirtualObjectMethodA(obj, jclass(getHandle()), method, (jvalue*)args);
		handleJavaExceptions();
		return toString(result);
	}

	template <> std::string Class::get(field_t field) const
	{
		return toString(env()->GetStaticObjectField(jclass(getHandle()), field));
	}

	template <> void Class::set(field_t field, const std::string& value)
	{
		JNIEnv* env = jni::env();

		jobject handle = env->NewStringUTF(value.c_str());
		env->SetStaticObjectField(jclass(getHandle()), field, handle);
		env->DeleteLocalRef(handle);
	}

	template <>	std::wstring Class::callStaticMethod(method_t method, internal::value_t* args) const
	{
		auto result = env()->CallStaticObjectMethodA(jclass(getHandle()), method, (jvalue*) args);
		handleJavaExceptions();
		return toWString(result);
	}

	template <>	std::wstring Class::callExactMethod(jobject obj, method_t method, internal::value_t* args) const
	{
		auto result = env()->CallNonvirtualObjectMethodA(obj, jclass(getHandle()), method, (jvalue*)args);
		handleJavaExceptions();
		return toWString(result);
	}

	template <> std::wstring Class::get(field_t field) const
	{
		return toWString(env()->GetStaticObjectField(jclass(getHandle()), field));
	}

#ifdef _WIN32

	template <> void Class::set(field_t field, const std::wstring& value)
	{
		JNIEnv* env = jni::env();

		jobject handle = env->NewString((const jchar*) value.c_str(), jsize(value.length()));
		env->SetStaticObjectField(jclass(getHandle()), field, handle);
		env->DeleteLocalRef(handle);
	}

#else
# error "utf32"
#endif

	Object Class::newObject(method_t constructor, internal::value_t* args) const
	{
		jobject ref = env()->NewObjectA(jclass(getHandle()), constructor, (jvalue*)args);
		handleJavaExceptions();
		return Object(ref, DeleteLocalInput);
	}

	/*
		Class Implementation
	 */

	typedef jint (JNICALL *CreateVm_t)(JavaVM**, void**, void*);


	static std::string detectJvmPath()
	{
		std::string result;

#ifdef _WIN32

		BYTE buffer[1024];
		DWORD size = sizeof(buffer);
		HKEY versionKey;

		if (::RegOpenKeyA(HKEY_LOCAL_MACHINE, "Software\\JavaSoft\\Java Runtime Environment\\", &versionKey) == ERROR_SUCCESS)
		{
			if (::RegQueryValueEx(versionKey, "CurrentVersion", NULL, NULL, buffer, &size) == ERROR_SUCCESS)
			{
				HKEY libKey;

				std::string keyName = std::string("Software\\JavaSoft\\Java Runtime Environment\\") + (const char*) buffer;

				::RegCloseKey(versionKey);

				if (::RegOpenKeyA(HKEY_LOCAL_MACHINE, keyName.c_str(), &libKey) == ERROR_SUCCESS)
				{
					size = sizeof(buffer);

					if (::RegQueryValueEx(libKey, "RuntimeLib", NULL, NULL, buffer, &size) == ERROR_SUCCESS)
					{
						result = (const char*) buffer;
					}

					::RegCloseKey(libKey);
				}
			}
		}

#endif // _WIN32

		return result;
	}

	Vm::Vm(const char* path_)
	{
		bool expected = false;

		std::string path = path_ ? path_ : detectJvmPath();

		if (path.length() == 0)
			throw InitializationException("Could not locate Java Virtual Machine");
		if (!isVm.compare_exchange_strong(expected, true))
			throw InitializationException("Java Virtual Machine already initialized");

		if (javaVm == nullptr)
		{
			JNIEnv* env;
			JavaVMInitArgs args = {};
			args.version = JNI_VERSION_1_2;

#ifdef _WIN32

			HMODULE lib = ::LoadLibraryA(path.c_str());

			if (lib == NULL)
			{
				size_t index = path.rfind("\\client\\");

				// JRE path names switched from "client" to "server" directory.
				if (index != std::string::npos)
				{
					path = path.replace(index, 8, "\\server\\");
					lib = ::LoadLibraryA(path.c_str());
				}

				if (lib == NULL)
				{
					isVm.store(false);
					throw InitializationException("Could not load JVM library");
				}
			}

			CreateVm_t JNI_CreateJavaVM = (CreateVm_t) ::GetProcAddress(lib, "JNI_CreateJavaVM");

			if (JNI_CreateJavaVM == NULL || JNI_CreateJavaVM(&javaVm, (void**) &env, &args) != 0)
			{
				isVm.store(false);
				::FreeLibrary(lib);
				throw InitializationException("Java Virtual Machine failed during creation");
			}

#else
# error Platform not yet supported
#endif // _WIN32
		}
	}

	Vm::~Vm()
	{
		/*
			Note that you can't ever *really* unload the JavaVM. If you call
			DestroyJavaVM(), you can't then call JNI_CreateJavaVM() again.
			So, instead we just flag it as "gone".
		 */
		isVm.store(false);
	}
}

