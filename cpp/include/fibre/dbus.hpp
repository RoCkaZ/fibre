#ifndef __FIBRE_DBUS_HPP
#define __FIBRE_DBUS_HPP

// helpful reference: http://www.matthew.ath.cx/misc/dbus

#include <dbus/dbus.h>
#include <unordered_map>
#include <vector>
#include <limits.h>
#include <string.h>
#include <fibre/worker.hpp>
#include <fibre/cpp_utils.hpp>
#include <fibre/print_utils.hpp>

namespace fibre {

/* Forward declarations ------------------------------------------------------*/

class DBusObject;

/**
 * @brief Implements message push/pop functions for each supported type.
 * 
 * This template is specialized for every type that this DBus wrapper supports.
 * 
 * Each specialization shall have the following static members:
 * - `type_id` shall be an integer_constant<int> holding a DBus type id
 * - `signature` shall be a static_string holding the full DBus signature of the type as string
 * - int push(DBusMessageIter* iter, T val) shall append the given value to a message
 * - int pop(DBusMessageIter* iter, T& val) shall dequeue a value of type T from the
 *   message and assign it to val.
 */
template<typename T, typename=std::true_type>
struct DBusTypeTraits;

template<size_t I, size_t N, typename T>
struct variant_helper;

// @brief A std::variant supporting the types most commonly used in DBus variants
using dbus_variant = std::variant<
    std::string, bool, DBusObject,
    /* int8_t (char on most platforms) is not supported by DBus */
    short, int, long, long long,
    unsigned char, unsigned short, unsigned int, unsigned long, unsigned long long,
    std::vector<std::string>
>;


/* Function definitions ------------------------------------------------------*/

/**
 * @brief Appends the given arguments to the message iterator.
 */
template<typename ... Ts>
static int pack_message(DBusMessageIter* iter, Ts ... args);

template<>
inline int pack_message(DBusMessageIter* iter) {
    return 0;
}

template<typename T, typename ... Ts>
inline int pack_message(DBusMessageIter* iter, T arg, Ts... args) {
    if (DBusTypeTraits<T>::push(iter, arg) != 0) {
        printf("Failed to pack message\n");
        return -1;
    }
    int result = pack_message<Ts...>(iter, args...);
    return result;
}


/**
 * @brief Unpacks the message into the given references.
 */
template<typename ... Ts>
static int unpack_message(DBusMessageIter* iter, Ts& ... args);

template<>
inline int unpack_message(DBusMessageIter* iter) {
    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_INVALID) {
        printf("Too many arguments\n");
        return -1;
    }
    return 0;
}

template<typename T, typename ... Ts>
inline int unpack_message(DBusMessageIter* iter, T& arg, Ts&... args) {
    if (dbus_message_iter_get_arg_type(iter) != DBusTypeTraits<T>::type_id::value) {
        printf("Argument type mismatch. Expected %d, got %d\n", DBusTypeTraits<T>::type_id::value, dbus_message_iter_get_arg_type(iter));
        return -1;
    }
    if (DBusTypeTraits<T>::pop(iter, arg) != 0) {
        printf("Failed to unpack message\n");
        return -1;
    }
    dbus_message_iter_next(iter);
    return unpack_message<Ts...>(iter, args...);
}

template<typename ... Ts, std::size_t... I>
int unpack_message_to_tuple_impl(DBusMessageIter* iter, std::tuple<Ts...>& tuple, std::index_sequence<I...>) {
    return unpack_message(iter, std::get<I>(tuple)...);
}

template<typename ... Ts>
int unpack_message_to_tuple(DBusMessageIter* iter, std::tuple<Ts...>& tuple) {
    return unpack_message_to_tuple_impl(iter, tuple, std::make_index_sequence<sizeof...(Ts)>());
}

template<typename ... TOutputs>
void handle_reply_message(DBusMessage* msg, Callback<TOutputs...>* callback) {
    DBusMessageIter args;
    dbus_message_iter_init(msg, &args);

    int type = dbus_message_get_type(msg);
    if (type == DBUS_MESSAGE_TYPE_ERROR) {
        std::string error_msg;
        if (unpack_message(&args, error_msg) != 0) {
            printf("Failed to unpack error message. Will not invoke callback.\n");
        }
        std::cerr << "DBus error received: " << error_msg;
        return;
    }
    if (type != DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        printf("the message is type %d. Will not invoke callback.\n", type);
        return;
    }
    std::tuple<TOutputs...> values;
    if (unpack_message_to_tuple(&args, values) != 0) {
        printf("Failed to unpack message. Will not invoke callback.\n");
        return;
    }
    printf("message unpacking complete\n");

    if (callback && callback->callback) {
        apply(callback->callback, std::tuple_cat(std::make_tuple(callback->ctx), values));
    }
}


/* Class definitions ---------------------------------------------------------*/

class DBusConnectionWrapper {
public:
    int init(Worker* worker);
    int deinit();

    DBusConnection* get_libdbus_ptr() { return conn_; }

private:
    int handle_add_watch(DBusWatch *watch);
    void handle_remove_watch(DBusWatch *watch);
    int handle_toggle_watch(DBusWatch *watch, bool enable);

    int handle_add_timeout(DBusTimeout* timeout);
    void handle_remove_timeout(DBusTimeout* timeout);
    int handle_toggle_timeout(DBusTimeout *timeout, bool enable);

    DBusError err_;
    DBusConnection* conn_;
    Worker* worker_;

    Signal dispatch_signal_ = Signal("dbus dispatch");
    Signal::callback_t dispatch_callback_obj_ = {
        .callback = [](void* ctx){ printf("dispatch\n"); dbus_connection_dispatch(((DBusConnectionWrapper*)ctx)->conn_); },
        .ctx = this
    };
};


class DBusObject {
public:
    DBusObject()
        : conn_(nullptr), service_name_(""), object_name_("") {}

    DBusObject(DBusConnectionWrapper* conn, std::string service_name, std::string object_name) 
        : conn_(conn), service_name_(service_name), object_name_(object_name) {}

    inline bool operator==(const DBusObject & other) const {
        return (conn_ == other.conn_) && (service_name_ == other.service_name_)
            && (object_name_ == other.object_name_);
    }

    inline bool operator!=(const DBusObject & other) const {
        return (conn_ != other.conn_) || (service_name_ != other.service_name_)
            || (object_name_ != other.object_name_);
    }

    template<typename ... TInputs, typename ... TOutputs>
    int method_call_async(const char* interface_name, const char* method_name, TInputs ... inputs, Callback<TOutputs...>* callback) {
        DBusMessage* msg;
        DBusMessageIter args;
        DBusPendingCall* pending;

        auto pending_call_handler = [](DBusPendingCall* pending, void* ctx){
            Callback<TOutputs...>* callback = reinterpret_cast<Callback<TOutputs...>*>(ctx);
            DBusMessage* msg = dbus_pending_call_steal_reply(pending);
            dbus_pending_call_unref(pending);
            handle_reply_message(msg, callback);
            dbus_message_unref(msg);
        };

        msg = dbus_message_new_method_call(service_name_.c_str(), // target for the method call
                object_name_.c_str(), // object to call on
                interface_name, // interface to call on
                method_name); // method name
        if (!msg) {
            fprintf(stderr, "Message Null\n");
            goto fail1;
        }

        dbus_message_iter_init_append(msg, &args);
        if (pack_message(&args, inputs...) != 0) {
            fprintf(stderr, "failed to pack args");
            goto fail2;
        }

        // send message and get a handle for a reply
        if (!dbus_connection_send_with_reply(conn_->get_libdbus_ptr(), msg, &pending, -1)) { // -1 is default timeout
            fprintf(stderr, "Out Of Memory!\n"); 
            goto fail2;
        }
        if (!pending) { 
            fprintf(stderr, "Pending Call Null\n"); 
            goto fail2; 
        }
        dbus_connection_flush(conn_->get_libdbus_ptr()); // TODO: not sure if we should flush here

        // free output message
        dbus_message_unref(msg);
        printf("Dispatched message call\n");

        if (!dbus_pending_call_set_notify(pending, pending_call_handler, callback, nullptr)) {
            printf("failed to set pending call callback\n");
            goto fail1;
        }

        // Handle reply now if it already arrived before we set the notify callback.
        msg = dbus_pending_call_steal_reply(pending);
        if (msg) {
            dbus_pending_call_unref(pending);
            handle_reply_message(msg, callback);
            dbus_message_unref(msg);
        }

        return 0;

fail2:
        dbus_message_unref(msg);
fail1:
        return -1;
    }

    DBusConnectionWrapper* conn_;
    std::string service_name_;
    std::string object_name_;
};


/* DBus Type Traits ----------------------------------------------------------*/

template<size_t BITS, bool SIGNED> constexpr int get_int_type_id();
template<> constexpr int get_int_type_id<16, true>() { return DBUS_TYPE_INT16; }
template<> constexpr int get_int_type_id<32, true>() { return DBUS_TYPE_INT32; }
template<> constexpr int get_int_type_id<64, true>() { return DBUS_TYPE_INT64; }
template<> constexpr int get_int_type_id<8, false>() { return DBUS_TYPE_BYTE; }
template<> constexpr int get_int_type_id<16, false>() { return DBUS_TYPE_UINT16; }
template<> constexpr int get_int_type_id<32, false>() { return DBUS_TYPE_UINT32; }
template<> constexpr int get_int_type_id<64, false>() { return DBUS_TYPE_UINT64; }

template<size_t BITS, bool SIGNED> constexpr static_string<1> get_int_signature();
template<> constexpr static_string<1> get_int_signature<16, true>() { return DBUS_TYPE_INT16_AS_STRING; }
template<> constexpr static_string<1> get_int_signature<32, true>() { return DBUS_TYPE_INT32_AS_STRING; }
template<> constexpr static_string<1> get_int_signature<64, true>() { return DBUS_TYPE_INT64_AS_STRING; }
template<> constexpr static_string<1> get_int_signature<8, false>() { return DBUS_TYPE_BYTE_AS_STRING; }
template<> constexpr static_string<1> get_int_signature<16, false>() { return DBUS_TYPE_UINT16_AS_STRING; }
template<> constexpr static_string<1> get_int_signature<32, false>() { return DBUS_TYPE_UINT32_AS_STRING; }
template<> constexpr static_string<1> get_int_signature<64, false>() { return DBUS_TYPE_UINT64_AS_STRING; }


template<typename T>
static inline const char* get_signature() {
    static auto sig = DBusTypeTraits<T>::signature;
    return sig.c_str();
}

template<typename T>
struct DBusTypeTraits<T, typename std::is_integral<T>::type> {
    static constexpr size_t BITS = sizeof(T) * CHAR_BIT;
    using type_id = std::integral_constant<int, get_int_type_id<BITS, std::is_signed<T>::value>()>;
    static constexpr auto signature = get_int_signature<BITS, std::is_signed<T>::value>();

    static int push(DBusMessageIter* iter, T val) {
        return dbus_message_iter_append_basic(iter, type_id::value, &val) ? 0 : -1;
    }

    static int pop(DBusMessageIter* iter, T& val) {
        dbus_message_iter_get_basic(iter, &val);
        return 0;
    }
};

template<>
struct DBusTypeTraits<bool> {
    using type_id = std::integral_constant<int, DBUS_TYPE_BOOLEAN>;
    static constexpr auto signature = make_const_string(DBUS_TYPE_BOOLEAN_AS_STRING);

    // BOOLEAN values are marshalled as 32-bit integers. Only 0 and 1 are valid.
    // Source: https://dbus.freedesktop.org/doc/dbus-specification.html#idm646

    static int push(DBusMessageIter* iter, bool val) {
        uint32_t val_as_int = val ? 1 : 0;
        return dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &val_as_int) ? 0 : -1;
    }

    static int pop(DBusMessageIter* iter, bool& val) {
        uint32_t val_as_int = 0;
        dbus_message_iter_get_basic(iter, &val_as_int);
        if (val_as_int == 0) {
            val = false;
        } else if (val_as_int == 1) {
            val = true;
        } else {
            printf("Invalid boolean value %d\n", val_as_int);
            return -1;
        }
        return 0;
    }
};

template<>
struct DBusTypeTraits<std::string> {
    using type_id = std::integral_constant<int, DBUS_TYPE_STRING>;
    static constexpr static_string<1> signature = DBUS_TYPE_STRING_AS_STRING;

    static int push(DBusMessageIter* iter, std::string val) {
        const char* str = val.c_str();
        int result = dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &str) ? 0 : -1;
        return result;
    }

    static int pop(DBusMessageIter* iter, std::string& val) {
        char* c_str = nullptr;
        dbus_message_iter_get_basic(iter, &c_str);
        if (c_str == nullptr) {
            printf("Popped invalid string\n");
            return -1;
        }
        val = std::string(c_str);
        return 0;
    }
};

/* TODO: a tuple should probably correspond to a DBus struct
template<typename ... TElement>
struct DBusTypeTraits<std::tuple<TElement...>> {
    using type_id = std::integral_constant<int, DBUS_TYPE_STRUCT>;

    static int pop(DBusMessageIter* iter, std::tuple<TElement...>& val) {
        DBusMessageIter subiter;
        dbus_message_iter_recurse(iter, &subiter);
        unpack_message_to_tuple(&subiter, val);
        return 0;
    }
};
*/

template<typename TElement>
struct DBusTypeTraits<std::vector<TElement>> {
    using type_id = std::integral_constant<int, DBUS_TYPE_ARRAY>;
    static constexpr auto signature = const_str_concat(make_const_string(DBUS_TYPE_ARRAY_AS_STRING), DBusTypeTraits<TElement>::signature);

    static int push(DBusMessageIter* iter, std::vector<TElement> val) {
        DBusMessageIter subiter;
        if (!dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, get_signature<TElement>(), &subiter)) {
            printf("failed to open container\n");
            return -1;
        }
        for (TElement& el: val) {
            if (DBusTypeTraits<TElement>::push(&subiter, el) != 0) {
                printf("failed to append array element\n");
                return -1;
            }
        }
        if (!dbus_message_iter_close_container(iter, &subiter)) {
            printf("failed to close container\n");
            return -1;
        }
        return 0;
    }

    static int pop(DBusMessageIter* iter, std::vector<TElement>& val) {
        printf("pop vector\n");
        DBusMessageIter subiter;
        dbus_message_iter_recurse(iter, &subiter);
        while (dbus_message_iter_get_arg_type(&subiter) != DBUS_TYPE_INVALID) {
            TElement element;
            if (DBusTypeTraits<TElement>::pop(&subiter, element) != 0)
                return -1;
            val.push_back(element);
            dbus_message_iter_next(&subiter);
        }
        return 0;
    }
};


template<typename TKey, typename TVal>
struct DBusTypeTraits<std::unordered_map<TKey, TVal>> {
    using type_id = std::integral_constant<int, DBUS_TYPE_ARRAY>;
    static constexpr auto element_signature = const_str_concat(
            make_const_string(DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING),
            DBusTypeTraits<TKey>::signature, DBusTypeTraits<TVal>::signature,
            make_const_string(DBUS_DICT_ENTRY_END_CHAR_AS_STRING));
    static constexpr auto signature = const_str_concat(
            make_const_string(DBUS_TYPE_ARRAY_AS_STRING),
            element_signature);

    static int push(DBusMessageIter* iter, std::unordered_map<TKey, TVal> val) {
        static auto elem_sig = element_signature;
        DBusMessageIter dict;
        if (!dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, elem_sig.c_str(), &dict)) {
            printf("failed to open dict container\n");
            return -1;
        }

        for (auto& it: val) {
            DBusMessageIter entry;
            if (!dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry)) {
                printf("failed to open dict entry container\n");
                return -1;
            }
            DBusTypeTraits<TKey>::push(&entry, it.first);
            DBusTypeTraits<TVal>::push(&entry, it.second);
            if (!dbus_message_iter_close_container(&dict, &entry)) {
                printf("failed to close container\n");
                return -1;
            }
        }

        if (!dbus_message_iter_close_container(iter, &dict)) {
            printf("failed to close container\n");
            return -1;
        }
        return 0;
    }

    static int pop(DBusMessageIter* iter, std::unordered_map<TKey, TVal>& val) {
        //val 
        DBusMessageIter dict;
        dbus_message_iter_recurse(iter, &dict);
        while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
            TKey dict_entry_key;
            TVal dict_entry_val;
            DBusMessageIter dict_entry;
            dbus_message_iter_recurse(&dict, &dict_entry);
            if (unpack_message(&dict_entry, dict_entry_key, dict_entry_val) != 0) {
                printf("failed to unpack dict entry\n");
                return -1;
            }
            val[dict_entry_key] = dict_entry_val;
            dbus_message_iter_next(&dict);
        }
        if (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
            printf("dict contains something else than dict entry\n");
            return -1;
        }
        return 0;
    }
};


template<>
struct DBusTypeTraits<DBusObject> {
    using type_id = std::integral_constant<int, DBUS_TYPE_OBJECT_PATH>;
    static constexpr static_string<1> signature = DBUS_TYPE_OBJECT_PATH_AS_STRING;

    static int push(DBusMessageIter* iter, DBusObject val) {
        const char * object_path = val.object_name_.c_str();
        return dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &object_path) ? 0 : -1;
    }

    static int pop(DBusMessageIter* iter, DBusObject& val) {
        std::string object_path;
        int result = DBusTypeTraits<std::string>::pop(iter, object_path);
        if (result == 0) {
            val = DBusObject(nullptr, "", object_path);
        }
        return result;
    }
};

/** @brief Helper class for pushing and popping variants */
template<size_t I, size_t N, typename ... Ts>
struct variant_helper<I, N, std::variant<Ts...>> {
    using subtype = std::tuple_element_t<I, std::tuple<Ts...>>;
    using next_helper = variant_helper<(I+1), N, std::variant<Ts...>>;

    static const char* get_element_signature(std::variant<Ts...>& val) {
        if (val.index() == I) {
            return get_signature<subtype>();
        }
        return next_helper::get_element_signature(val);
    }

    static int push(DBusMessageIter* iter, std::variant<Ts...>& val) {
        if (val.index() == I) {
            return DBusTypeTraits<subtype>::push(iter, std::get<I>(val));
        }
        return next_helper::push(iter, val);
    }

    static int pop(DBusMessageIter* iter, std::variant<Ts...>& val, const char* signature) {
        if (strcmp(get_signature<subtype>(), signature) == 0) {
            subtype inner_val{};
            int result = DBusTypeTraits<subtype>::pop(iter, inner_val);
            val = std::variant<Ts...>(inner_val);
            return result;
        }
        return next_helper::pop(iter, val, signature);
    }

};

/** @brief Helper class for pushing and popping variants */
template<size_t I, typename ... Ts>
struct variant_helper<I, I, std::variant<Ts...>> {
    static const char* get_element_signature(std::variant<Ts...>& val) {
        printf("variant implementation broken\n");
        return nullptr;
    }

    static int push(DBusMessageIter* iter, std::variant<Ts...>& val) {
        printf("variant implementation broken\n");
        return -1;
    }

    static int pop(DBusMessageIter* iter, std::variant<Ts...>& val, const char* signature) {
        printf("signature %s not supported by this variant implementation\n", signature);
        return -1;
    }
};

template<typename ... Ts>
struct DBusTypeTraits<std::variant<Ts...>> {
    using type_id = std::integral_constant<int, DBUS_TYPE_VARIANT>;
    static constexpr static_string<1> signature = DBUS_TYPE_VARIANT_AS_STRING;

    using helper = variant_helper<0, sizeof...(Ts), std::variant<Ts...>>;

    static int push(DBusMessageIter* iter, std::variant<Ts...>& val) {
        DBusMessageIter subiter;
        if (!dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, helper::get_element_signature(val), &subiter)) {
            printf("failed to open container\n");
            return -1;
        }
        int result = helper::push(&subiter, val);
        if (!dbus_message_iter_close_container(iter, &subiter)) {
            printf("failed to close container\n");
            result = -1;
        }
        return result;
    }

    static int pop(DBusMessageIter* iter, std::variant<Ts...>& val) {
        DBusMessageIter subiter;
        dbus_message_iter_recurse(iter, &subiter);
        char* signature = dbus_message_iter_get_signature(&subiter);
        int result = helper::pop(&subiter, val, signature);
        dbus_free(signature);
        return result;
    }
};


}


/* Extensions for std namespace ----------------------------------------------*/

namespace std {

template<typename ... Ts>
static std::ostream& operator<<(std::ostream& stream, const fibre::DBusObject& val) {
    return stream << "DBusObject(" << val.service_name_ << ", " << val.object_name_ << ")";
}

template<>
struct hash<fibre::DBusObject> {
    size_t operator()(const fibre::DBusObject& obj) const {
        return hash<uintptr_t>()(reinterpret_cast<uintptr_t>(obj.conn_)) +
               hash<string>()(obj.service_name_) +
               hash<string>()(obj.object_name_);
    }
};

}

#endif // __FIBRE_DBUS_HPP