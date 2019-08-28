/*
* Implements types and macros needed to export local function to remote Fibre
* nodes.
* This file is analogous and similar in structure to local_ref_types.hpp.
*/
#ifndef __FIBRE_LOCAL_ENDPOINT
#define __FIBRE_LOCAL_ENDPOINT

//#ifndef __FIBRE_HPP
//#error "This file should not be included directly. Include fibre.hpp instead."
//#endif
#include <fibre/fibre.hpp>

namespace fibre {

class LocalEndpoint {
public:
    //virtual void invoke(StreamSource* input, StreamSink* output) = 0;
    virtual void open_connection(IncomingConnectionDecoder& input) const = 0;
    virtual void decoder_finished(const IncomingConnectionDecoder& input, OutputPipe* output) const = 0;
    virtual uint16_t get_hash() const = 0;
    virtual bool get_as_json(const char ** output, size_t* length) const = 0;
};

template<typename... Ts>
class Decoder;

template<>
class Decoder<uint32_t> : public FixedIntDecoder<uint32_t, false> {
public:
    static constexpr auto name = make_sstring("uint32");
    using TName = decltype(name);
    using value_tuple_t = std::tuple<uint32_t>;
};

template<typename TObj>
class Decoder<TObj*> : public ObjectReferenceDecoder<TObj> {
public:
    //static constexpr auto name = make_sstring("lalala");
    static constexpr auto name = type_name_provider<TObj>::get_type_name(); //make_sstring("lalala");
    using TName = decltype(name);
    using value_tuple_t = std::tuple<TObj*>;
};

struct get_value_functor {
    template<typename T>
    decltype(std::declval<T>().get_value()) operator () (T&& t) {
        return t.get_value();
    }
};

template<typename... TDecoders>
class DecoderChain : public StaticStreamChain<TDecoders...> {
public:
    using decoder_tuple_t = std::tuple<TDecoders...>;
    using value_tuple_t = tuple_cat_t<typename TDecoders::value_tuple_t...>;
    static value_tuple_t get_inputs(const DecoderChain& decoder) {
        return for_each_in_tuple(get_value_functor(), std::forward<const decoder_tuple_t>(decoder.get_all_streams()));
    }
};

template<typename... Ts>
class Encoder;

template<typename... Ts>
class VoidEncoder {
public:
    static void serialize(StreamSink* output, Ts&&... value) {
        // nothing to do
    }
};

template<>
class Encoder<uint32_t> {
public:
    static void serialize(StreamSink* output, uint32_t&& value) {
        uint8_t buf[4];
        write_le<uint32_t>(value, buf);
        output->process_bytes(buf, sizeof(buf), nullptr);
    }
};

template<>
class Encoder<const char *, size_t> {
public:
    static void serialize(StreamSink* output, const char * str, size_t&& length) {
        uint32_t i = str ? length : 0;
        size_t processed_bytes = 0;
        uint8_t buf[4];
        LOG_FIBRE(SERDES, "will encode string of length ", length);
        write_le<uint32_t>(i, buf);
        StreamSink::status_t status = output->process_bytes(buf, sizeof(buf), &processed_bytes);
        if (status != StreamSink::OK || processed_bytes != sizeof(buf)) {
            LOG_FIBRE_W(SERDES, "not everything processed");
            return;
        }

        if (str) {
            processed_bytes = 0;
            StreamSink::status_t status = output->process_bytes(reinterpret_cast<const uint8_t*>(str), length, &processed_bytes);
            if (processed_bytes != length) {
                LOG_FIBRE_W(SERDES, "not everything processed: ", processed_bytes);
            }
            if (status != StreamSink::OK) {
                LOG_FIBRE_W(SERDES, "error in output");
            }
        } else {
            LOG_FIBRE_W(SERDES, "attempt to serialize null string");
        }
    }
};

template<typename... TEncoders>
class EncoderChain;

template<>
class EncoderChain<> {
public:
    using value_tuple_t = std::tuple<>;
    static void serialize(StreamSink* output, value_tuple_t values) {
        // nothing to do
    }
};

template<template<typename... Ts> typename TEncoder, typename... Ts, typename... TEncoders>
class EncoderChain<TEncoder<Ts...>, TEncoders...> {
public:
    using value_tuple_t = tuple_cat_t<std::tuple<Ts...>, typename EncoderChain<TEncoders...>::value_tuple_t>;
    static void serialize(StreamSink* output, value_tuple_t values) {
        std::apply(TEncoder<Ts...>::serialize, std::tuple_cat(std::make_tuple(output), tuple_take<sizeof...(Ts)>(values)));
        EncoderChain<TEncoders...>::serialize(output, tuple_skip<sizeof...(Ts)>(values));
    }
};


/**
 * @brief Assembles a JSON snippet that describes a function
 */
template<typename TMetadata>
struct FunctionJSONAssembler {
private:
    template<size_t I>
    using get_input_json_t = sstring<22 +
                std::tuple_element_t<I, typename TMetadata::TInputMetadata>::TName::size() +
                decltype(std::tuple_element_t<I, typename TMetadata::TInputMetadata>::decoder_type::name)::size()
            >;

    template<size_t I>
    static constexpr get_input_json_t<I>
    get_input_json(const TMetadata& metadata) {
        using TElement = std::tuple_element_t<I, typename TMetadata::TInputMetadata>;
        using TDecoder = typename TElement::decoder_type;
        return make_sstring("{\"name\":\"") +
               std::get<I>(metadata.get_input_metadata()).name +
               make_sstring("\",\"codec\":\"") +
               TDecoder::name +
               make_sstring("\"}");
    }

    template<typename Is>
    struct get_all_inputs_json_type;
    template<size_t... Is>
    struct get_all_inputs_json_type<std::index_sequence<Is...>> {
        using type = join_sstring_t<1, get_input_json_t<Is>::size()...>;
    };
    using get_all_inputs_json_t = typename get_all_inputs_json_type<std::make_index_sequence<TMetadata::NInputs>>::type;

    template<size_t... Is>
    static constexpr get_all_inputs_json_t
    get_all_inputs_json(const TMetadata& metadata, std::index_sequence<Is...>) {
        return join_sstring(make_sstring(","), get_input_json<Is>(metadata)...);
    }

public:
    using get_json_t = sstring<19 + TMetadata::TFuncName::size() + get_all_inputs_json_t::size()>;

    // @brief Returns a JSON snippet that describes this function
    static constexpr get_json_t
    get_as_json(const TMetadata& metadata) {
        return make_sstring("{\"name\":\"") +
               metadata.get_function_name() +
               make_sstring("\",\"in\":[") +
               get_all_inputs_json(metadata, std::make_index_sequence<TMetadata::NInputs>()) +
               make_sstring("]}");
    }
};


template<size_t INameLength, size_t IInParams>
struct InputMetadataPrototype {
    sstring<INameLength> name;
};

template<size_t INameLength, typename TArgs>
struct InputMetadata;

template<size_t INameLength, typename... TArgs>
struct InputMetadata<INameLength, std::tuple<TArgs...>> {
    using TName =  sstring<INameLength>;
    TName name;
    using decoder_type = Decoder<TArgs...>;
    using tuple_type = std::tuple<TArgs...>;
};

template<size_t INameLength, size_t IOutParams, bool BDiscard>
struct OutputMetadataPrototype {
    sstring<INameLength> name;
};

template<size_t INameLength, typename TArgs, bool BDiscard>
struct OutputMetadata;

template<size_t INameLength, typename... TArgs, bool BDiscard>
struct OutputMetadata<INameLength, std::tuple<TArgs...>, BDiscard> {
    using TName =  sstring<INameLength>;
    TName name;
    using encoder_type = std::conditional_t<
            BDiscard,
            VoidEncoder<typename remove_ref_or_ptr<TArgs>::type...>,
            Encoder<typename remove_ref_or_ptr<TArgs>::type...>
        >;
    using tuple_type = std::tuple<TArgs...>;
};

template<size_t IInParams, size_t INameLengthPlus1>
constexpr InputMetadataPrototype<(INameLengthPlus1-1), IInParams> make_input_metadata_prototype(const char (&name)[INameLengthPlus1]) {
    return InputMetadataPrototype<(INameLengthPlus1-1), IInParams>{
        make_sstring(name)
    };
}

template<size_t IOutParams, bool BDiscard, size_t INameLengthPlus1>
constexpr OutputMetadataPrototype<(INameLengthPlus1-1), IOutParams, BDiscard> make_output_metadata_prototype(const char (&name)[INameLengthPlus1]) {
    return OutputMetadataPrototype<(INameLengthPlus1-1), IOutParams, BDiscard>{
        make_sstring(name)
    };
}

struct arg_mode_input {};
struct arg_mode_output {};
struct arg_mode_return_value {};


template<typename TInputs, typename TOutputs, typename TArgModes>
struct merged_io_tuple_type;

template<typename... TInputs, typename... TOutputs>
struct merged_io_tuple_type<std::tuple<TInputs...>, std::tuple<TOutputs...>, std::tuple<>> {
    using type = std::tuple<>;
};

template<typename TInput, typename... TInputs, typename... TOutputs, typename... TArgModes>
struct merged_io_tuple_type<std::tuple<TInput, TInputs...>, std::tuple<TOutputs...>, std::tuple<arg_mode_input, TArgModes...>> {
    using tail = typename merged_io_tuple_type<std::tuple<TInputs...>, std::tuple<TOutputs...>, std::tuple<TArgModes...>>::type;
    using type = tuple_cat_t<std::tuple<TInput>, tail>;
};

template<typename... TInputs, typename TOutput, typename... TOutputs, typename... TArgModes>
struct merged_io_tuple_type<std::tuple<TInputs...>, std::tuple<TOutput, TOutputs...>, std::tuple<arg_mode_output, TArgModes...>> {
    using tail = typename merged_io_tuple_type<std::tuple<TInputs...>, std::tuple<TOutputs...>, std::tuple<TArgModes...>>::type;
    using type = tuple_cat_t<std::tuple<TOutput>, tail>;
};

template<typename TInputs, typename TOutputs, typename TArgModes>
using merged_io_tuple_t = typename merged_io_tuple_type<TInputs, TOutputs, TArgModes>::type;


template<typename TInputRefs, typename TOutputRefs>
std::tuple<>
merge_to_io_tuple(TInputRefs&& in_refs, TOutputRefs&& out_refs, std::tuple<> arg_modes) {
    return std::make_tuple();
}

template<typename TInputRefs, typename TOutputRefs, typename... TArgModes>
merged_io_tuple_t<TInputRefs, TOutputRefs, std::tuple<arg_mode_input, TArgModes...>>
merge_to_io_tuple(TInputRefs&& in_refs, TOutputRefs&& out_refs, std::tuple<arg_mode_input, TArgModes...> arg_modes) {
    using TFirst = std::tuple_element_t<0, TInputRefs>;
    return std::tuple_cat(
        std::tuple<TFirst>(std::get<0>(in_refs)),
        merge_to_io_tuple<tuple_skip_t<1, TInputRefs>, TOutputRefs>(
            tuple_skip<1>(in_refs),
            std::forward<TOutputRefs>(out_refs),
            tuple_skip<1>(arg_modes)
        )
    );
}

template<typename TInputRefs, typename TOutputRefs, typename... TArgModes>
merged_io_tuple_t<TInputRefs, TOutputRefs, std::tuple<arg_mode_output, TArgModes...>>
merge_to_io_tuple(TInputRefs&& in_refs, TOutputRefs&& out_refs, std::tuple<arg_mode_output, TArgModes...> arg_modes) {
    using TFirst = std::tuple_element_t<0, TOutputRefs>;
    return std::tuple_cat(
        std::tuple<TFirst>{std::get<0>(out_refs)},
        merge_to_io_tuple<TInputRefs, tuple_skip_t<1, TOutputRefs>>(
            std::forward<TInputRefs>(in_refs),
            tuple_skip<1>(out_refs),
            tuple_skip<1>(arg_modes)
        )
    );
}


template<typename TFuncName, typename TInputMetadata, typename TOutputMetadata, typename TFreeArgs, typename TArgModes>
struct StaticFunctionMetadata;

template<size_t IFun, typename... TInputs, typename... TOutputs, typename... TFreeArgs, typename... ArgModes>
struct StaticFunctionMetadata<
        sstring<IFun>,
        std::tuple<TInputs...>,
        std::tuple<TOutputs...>,
        std::tuple<TFreeArgs...>,
        std::tuple<ArgModes...>> {
    static constexpr const size_t NInputs = (sizeof...(TInputs));
    static constexpr const size_t NOutputs = (sizeof...(TOutputs));
    using TFuncName = sstring<IFun>;
    using TInputMetadata = std::tuple<TInputs...>;
    using TOutputMetadata = std::tuple<TOutputs...>;

    using TInputDecoders = DecoderChain<typename TInputs::decoder_type...>;
    using TOutputEncoders = EncoderChain<typename TOutputs::encoder_type...>;

    using TFreeArgsT = std::tuple<TFreeArgs...>;
    using TArgModes = std::tuple<ArgModes...>;

    using impl_in_vals_t = tuple_cat_t<typename TInputs::tuple_type...>;
    using impl_out_refs_t = tuple_cat_t<typename TOutputs::tuple_type...>;
    using impl_out_vals_t = remove_refs_or_ptrs_from_tuple_t<impl_out_refs_t>;
    using impl_io_refs_t = merged_io_tuple_t<impl_in_vals_t, impl_out_refs_t, TArgModes>;

    TFuncName function_name;
    TInputMetadata input_metadata;
    TOutputMetadata output_metadata;

    constexpr StaticFunctionMetadata(TFuncName function_name, TInputMetadata input_metadata, TOutputMetadata output_metadata)
        : function_name(function_name), input_metadata(input_metadata), output_metadata(output_metadata) {}

    template<size_t INameLength, size_t IInParams>
    constexpr StaticFunctionMetadata<TFuncName, tuple_cat_t<TInputMetadata, std::tuple<InputMetadata<INameLength, tuple_take_t<IInParams, TFreeArgsT>>>>, TOutputMetadata, tuple_skip_t<IInParams, TFreeArgsT>, tuple_cat_t<TArgModes, repeat_t<IInParams, arg_mode_input>>> with_item(InputMetadataPrototype<INameLength, IInParams> item) {
        return StaticFunctionMetadata<TFuncName, tuple_cat_t<TInputMetadata, std::tuple<InputMetadata<INameLength, tuple_take_t<IInParams, TFreeArgsT>>>>, TOutputMetadata, tuple_skip_t<IInParams, TFreeArgsT>, tuple_cat_t<TArgModes, repeat_t<IInParams, arg_mode_input>>>(
            function_name,
            std::tuple_cat(input_metadata, std::make_tuple(InputMetadata<INameLength, tuple_take_t<IInParams, TFreeArgsT>>{item.name})),
            output_metadata);
    }

    template<size_t INameLength, size_t IOutParams, bool BDiscard>
    constexpr StaticFunctionMetadata<TFuncName, TInputMetadata, tuple_cat_t<TOutputMetadata, std::tuple<OutputMetadata<INameLength, tuple_take_t<IOutParams, TFreeArgsT>, BDiscard>>>, tuple_skip_t<IOutParams, TFreeArgsT>, tuple_cat_t<TArgModes, repeat_t<IOutParams, arg_mode_output>>> with_item(OutputMetadataPrototype<INameLength, IOutParams, BDiscard> item) {
        return StaticFunctionMetadata<TFuncName, TInputMetadata, tuple_cat_t<TOutputMetadata, std::tuple<OutputMetadata<INameLength, tuple_take_t<IOutParams, TFreeArgsT>, BDiscard>>>, tuple_skip_t<IOutParams, TFreeArgsT>, tuple_cat_t<TArgModes, repeat_t<IOutParams, arg_mode_output>>>(
            function_name,
            input_metadata,
            std::tuple_cat(output_metadata, std::make_tuple(OutputMetadata<INameLength, tuple_take_t<IOutParams, TFreeArgsT>, BDiscard>{item.name})));
    }

    constexpr StaticFunctionMetadata with_items() {
        return *this;
    }

    template<typename T, typename ... Ts>
    constexpr auto with_items(T item, Ts... items) 
            -> decltype(with_item(item).with_items(items...)) {
        return with_item(item).with_items(items...);
    }

    constexpr TFuncName get_function_name() { return function_name; }
    constexpr TInputMetadata get_input_metadata() { return input_metadata; }
    constexpr TOutputMetadata get_output_metadata() { return output_metadata; }

    using JSONAssembler = FunctionJSONAssembler<StaticFunctionMetadata>;
    typename JSONAssembler::get_json_t json = JSONAssembler::get_as_json(*this);
};


template<typename TFreeArgs, size_t INameLength_Plus1>
static constexpr StaticFunctionMetadata<sstring<INameLength_Plus1-1>, std::tuple<>, std::tuple<>, TFreeArgs, std::tuple<>> make_function_metadata(const char (&function_name)[INameLength_Plus1]) {
    return StaticFunctionMetadata<sstring<INameLength_Plus1-1>, std::tuple<>, std::tuple<>, TFreeArgs, std::tuple<>>(sstring<INameLength_Plus1-1>(function_name), std::tuple<>(), std::tuple<>());
}



#if 0

template<typename, size_t NInputs, typename... Ts>
struct decoder_type;

template<size_t NInputs>
struct decoder_type<void, NInputs> {
    static_assert(NInputs == 0, "too many input names provided");
    using type = std::tuple<>;
    using remainder = std::tuple<>;
};

template<typename T, typename... Ts>
struct decoder_type<typename std::enable_if_t<(!is_tuple<T>::value)>, 0, T, Ts...> {
    using type = std::tuple<>;
    using remainder = std::tuple<T, Ts...>;
};

template<size_t NInputs, typename... TTuple, typename... Ts>
struct decoder_type<void, NInputs, std::tuple<TTuple...>, Ts...> {
private:
    using unpacked = decoder_type<void, NInputs, TTuple..., Ts...>;
public:
    using type = typename unpacked::type;
    using remainder = typename unpacked::remainder;
};

template<size_t NInputs, typename... Ts>
struct decoder_type<typename std::enable_if_t<(NInputs > 0)>, NInputs, uint32_t, Ts...> {
private:
    using tail = decoder_type<void, NInputs - 1, Ts...>;
    using this_type = FixedIntDecoder<uint32_t, false>;
public:
    using type = tuple_cat_t<std::tuple<this_type>, typename tail::type>;
    using remainder = typename tail::remainder;
};

template<size_t NInputs, typename TObj, typename... Ts>
struct decoder_type<typename std::enable_if<(NInputs > 0)>::type, NInputs, TObj*, Ts...> {
private:
    using tail = decoder_type<void, NInputs - 1, Ts...>;
    using this_type = ObjectReferenceDecoder<TObj>;
public:
    using type = tuple_cat_t<std::tuple<this_type>, typename tail::type>;
    using remainder = typename tail::remainder;
};

template<typename, size_t NOutputs, typename... Ts>
struct encoder_type;

template<size_t NOutputs>
struct encoder_type<void, NOutputs> {
    static_assert(NOutputs == 0, "too many output names provided");
    using type = std::tuple<>;
    using remainder = std::tuple<>;
    static void serialize(StreamSink* output) {
        // nothing to do
    }
};

template<typename T, typename... Ts>
struct encoder_type<void, 0, T, Ts...> {
    using type = std::tuple<>;
    using remainder = std::tuple<T, Ts...>;
    static void serialize(StreamSink* output, T&& value, Ts&&... tail_values) {
        // nothing to do
    }
};

template<size_t NOutputs, typename... TTuple, typename... Ts>
struct encoder_type<typename std::enable_if_t<(NOutputs > 0)>, NOutputs, std::tuple<TTuple...>, Ts...> {
private:
    using unpacked = encoder_type<void, NOutputs, TTuple..., Ts...>;
public:
    //using type = typename unpacked::type;
    //using remainder = typename unpacked::remainder;

    template<size_t... Is>
    static void serialize_impl(std::index_sequence<Is...>, StreamSink* output, std::tuple<TTuple...>&& val, Ts&&... tail_values) {
        unpacked::serialize(output, std::forward<TTuple>(std::get<Is>(val))..., std::forward<Ts>(tail_values)...);
    }
    static void serialize(StreamSink* output, std::tuple<TTuple...>&& val, Ts&&... tail_values) {
        serialize_impl(std::make_index_sequence<sizeof...(TTuple)>(), output, std::forward<std::tuple<TTuple...>>(val), std::forward<Ts>(tail_values)...);
    }
};

template<size_t NOutputs, typename... Ts>
struct encoder_type<typename std::enable_if_t<(NOutputs > 0)>, NOutputs, uint32_t, Ts...> {
private:
    using tail = encoder_type<void, NOutputs - 1, Ts...>;
public:
    //using type = tuple_cat_t<std::tuple<this_type>, typename tail::type>;
    //using remainder = typename tail::remainder;

    static void serialize(StreamSink* output, uint32_t&& value, Ts&&... tail_values) {
        uint8_t buf[4];
        write_le<uint32_t>(value, buf);
        output->process_bytes(buf, sizeof(buf), nullptr);
        tail::serialize(output, std::forward<Ts>(tail_values)...);
    }
};

template<size_t NOutputs, typename... Ts>
struct encoder_type<typename std::enable_if_t<(NOutputs > 0)>, NOutputs, const char *, size_t, Ts...> {
private:
    using tail = encoder_type<void, NOutputs - 1, Ts...>;
    using this_data_type = std::tuple<const char *, size_t>;
public:
    //using data_type = tuple_cat_t<this_data_type, typename tail::data_type>;
    //using remainder = typename tail::remainder;

    //static void serialize(data_type values, StreamSink* output) {
    //    serialize(&values.get<0>(output), &values.get<1>(output), output);
    //}

    static void serialize(StreamSink* output, const char * (&&str), size_t&& length, Ts&&... tail_values) {
        uint32_t i = str ? length : 0;
        size_t processed_bytes = 0;
        uint8_t buf[4];
        write_le<uint32_t>(i, buf, sizeof(buf));
        StreamSink::status_t status = output->process_bytes(buf, sizeof(buf), &processed_bytes);
        if (status != StreamSink::OK || processed_bytes != sizeof(buf)) {
            LOG_FIBRE_W(SERDES, "not everything processed");
            return;
        }

        if (str) {
            processed_bytes = 0;
            StreamSink::status_t status = output->process_bytes(reinterpret_cast<const uint8_t*>(str), length, &processed_bytes);
            if (processed_bytes != length) {
                LOG_FIBRE_W(SERDES, "not everything processed: ", processed_bytes);
            }
            if (status != StreamSink::OK) {
                LOG_FIBRE_W(SERDES, "error in output");
            }
        } else {
            LOG_FIBRE_W(SERDES, "attempt to serialize null string");
        }
        tail::serialize(output, std::forward<Ts>(tail_values)...);
    }
};



template<typename TStreamDecoders>
struct stream_decoder_chain_from_tuple;

template<typename... TStreamDecoders>
struct stream_decoder_chain_from_tuple<std::tuple<TStreamDecoders...>> {
    using type = StaticStreamChain<TStreamDecoders...>;
    using tuple_type = std::tuple<TStreamDecoders...>;
    using input_tuple_t = decltype(for_each_in_tuple(get_value_functor(), std::forward<const tuple_type>(std::declval<type>().get_all_streams())));
    static input_tuple_t get_inputs(const type& decoder) {
        return for_each_in_tuple(get_value_functor(), std::forward<const tuple_type>(decoder.get_all_streams()));
    }
};

template<typename TStreamDecoders>
struct encoder_chain_from_tuple;

template<typename... TEncoders>
struct encoder_chain_from_tuple<std::tuple<TEncoders...>> {
    using type = StaticStreamChain<TEncoders...>;
    using tuple_type = std::tuple<TEncoders...>;
    using input_tuple_t = decltype(for_each_in_tuple(get_value_functor(), std::forward<const tuple_type>(std::declval<type>().get_all_streams())));
    static input_tuple_t get_inputs(const type& decoder) {
        return for_each_in_tuple(get_value_functor(), std::forward<const tuple_type>(decoder.get_all_streams()));
    }
};
#endif

template<
    typename TFunc,
    //typename TFuncSignature,
    typename TMetadata>
class LocalFunctionEndpoint : public LocalEndpoint {
    //using TDecoders = decoder_type<void, TMetadata::NInputs, args_of_t<TFunc>>;

    static constexpr const size_t NImplInputs = std::tuple_size<typename TMetadata::impl_in_vals_t>::value;
    static constexpr const size_t NImplOutputs = std::tuple_size<typename TMetadata::impl_out_vals_t>::value;
    static constexpr const size_t NImplArgs = std::tuple_size<args_of_t<TFunc>>::value;
    static constexpr const size_t NImplResults = std::tuple_size<as_tuple_t<result_of_t<TFunc>>>::value;
    
    static_assert(NImplInputs + NImplOutputs == NImplArgs + NImplResults, "number of I/O values dont match for function and metadata");

    using impl_in_vals_t = typename TMetadata::impl_in_vals_t;
    //using impl_in_refs_t = add_refs_to_tuple_t<typename TMetadata::impl_in_vals_t>;
    //using output_arg_refs_t = tuple_take_t<NImplOutputs-NImplResults, typename TMetadata::outputs_t>;
    //using output_arg_vals_t = remove_refs_or_ptrs_from_tuple_t<output_arg_refs_t>;
    using impl_out_arg_vals_t = tuple_take_t<NImplOutputs-NImplResults, typename TMetadata::impl_out_vals_t>;
    using impl_out_arg_refs_t = tuple_take_t<NImplOutputs-NImplResults, typename TMetadata::impl_out_refs_t>;
    using impl_out_ret_vals_t = as_tuple_t<result_of_t<TFunc>>;
    using impl_arg_refs_t = tuple_take_t<NImplArgs, typename TMetadata::impl_io_refs_t>;
    using impl_arg_modes_t = tuple_take_t<NImplArgs, typename TMetadata::TArgModes>;

    //static_assert(tuple_skip_t<NImplArgs, typename TMetadata::TArgModes> TODO: check that all return values are indeed used as outputs

    //using arg_refs_t = tuple_take_t<std::tuple_size<args_of_t<TFunc>>::value, io_t>;
    //using ret_vals_t = tuple_take_t<std::tuple_size<args_of_t<TFunc>>::value, io_t>;

    //using out_arg_refs_t = typename TDecoders::remainder;
    //using out_arg_vals_t = remove_refs_or_ptrs_from_tuple_t<out_arg_refs_t>;

    //using TArgEncoders = encoder_type<void, TMetadata::NOutputs, out_arg_vals_t>;

    //using TRetEncoders = encoder_type<TMetadata::NOutputs, tuple_cat_t<out_arg_vals_t, as_tuple_t<result_of_t<TFunc>>>>;
    //int a =  TEncoders();
    //static_assert(
    //        (std::tuple_size<TEncoder::remainder> == 0) ||
    //        (std::tuple_size<TEncoder::remainder> == 1), "number of ");

    //using TDecoder = typename decoder_type<args_of_t<TFunc>>::type;
    //using TEncoder = typename decoder_type<args_of_t<TFunc>>::type;
    //static_assert(
    //    std::tuple_size<args_of_t<TFunc>> == TMetadata::NInputs,
    //    std::tuple_size<args_of_t<TFunc>> == TMetadata::NOu,
    //)
    //constexpr std::tuple_size<args_of_t<TFunc>> == NInputs

public:
    //constexpr LocalFunctionEndpoint(TMetadata metadata) : metadata_(metadata), json_(json) {}
    constexpr LocalFunctionEndpoint(TFunc&& func, TMetadata&& metadata) :
        func_(std::forward<TFunc>(func)),
        metadata_(std::forward<TMetadata>(metadata)) {}

    //using stream_chain = typename stream_decoder_chain_from_tuple<typename TDecoders::type>::type;
    using stream_chain = typename TMetadata::TInputDecoders;

    void open_connection(IncomingConnectionDecoder& incoming_connection_decoder) const final {
        incoming_connection_decoder.set_stream<stream_chain>();
    }

    /*
    void decoder_finished(const IncomingConnectionDecoder& incoming_connection_decoder, OutputPipe* output) const final {

//        TArgDecoders = decoders_from_metadata<FunctionMetadata>
//        TArgEncoders = encoders_from_metadata<FunctionMetadata>
//
//
//        auto in_and_out_refs = metadata_to_arg_tuple_t<FunctionMetadata, TEnc, TDec>
//

        const stream_chain* decoder = incoming_connection_decoder.get_stream<stream_chain>();
        LOG_FIBRE(INPUT, "received all function arguments");
        //printf("arg decoder finished");
        //using tuple_type = std::tuple<FixedIntDecoder<TInputs, false>...>;
        //std::tuple<const TInputs&...> inputs = for_each_in_tuple(get_value_functor(), std::forward<const tuple_type>(decoder->get_all_streams()));
        auto in_refs = stream_decoder_chain_from_tuple<typename TDecoders::type>::get_inputs(*decoder);
        out_arg_vals_t out_vals;
        out_arg_refs_t out_refs =
            add_ref_or_ptr_to_tuple<out_arg_refs_t>::convert(std::forward<out_arg_vals_t>(out_vals));
        //auto outputs = eam_detfrom_tuple<typename TEncoders::type>::get_outputs();
        //std::tuple<TOutputs...> outputs;
        //std::tuple<TOutputs&...> output_refs = std::forward_as_tuple(outputs);
        //std::tuple<TOutputs&...> output_refs(std::get<0>(outputs));
        auto in_and_out_refs = std::tuple_cat(in_refs, out_refs);
        std::apply(func_, in_and_out_refs);
        //TArgEncoders::serialize(output, std::forward<out_arg_vals_t>(out_vals));


        //printf("first arg is ", std::hex, std::get<0>(inputs));
        //LOG_FIBRE(SERDES, "will write output of type ", typeid(TOutputs).name()...);
        //Serializer<std::tuple<TOutputs...>>::serialize(outputs, output);
    }
*/

    void decoder_finished(const IncomingConnectionDecoder& incoming_connection_decoder, OutputPipe* output) const final {
        const stream_chain* decoder = incoming_connection_decoder.get_stream<stream_chain>();
        LOG_FIBRE(INPUT, "received all function arguments");

        // build tuple with references to all input values
        impl_in_vals_t in_vals = stream_chain::get_inputs(*decoder);

        // allocate tuple that holds all output values, except those that come as a return value
        impl_out_arg_vals_t out_arg_vals;
        impl_out_arg_refs_t out_arg_refs = 
                add_ref_or_ptr_to_tuple<impl_out_arg_refs_t>::convert(std::forward<impl_out_arg_vals_t>(out_arg_vals));

        // build tuple of references to inputs and outputs, arranged in the same order as they occur in the function signature
        impl_arg_refs_t in_and_out_refs = merge_to_io_tuple<impl_in_vals_t, impl_out_arg_refs_t>(std::forward<impl_in_vals_t>(in_vals), std::forward<impl_out_arg_refs_t>(out_arg_refs), impl_arg_modes_t{});
        
        // call the function by passing tuple's content as arguments
        as_tuple_t<result_of_t<TFunc>> out_ret_vals = std::apply(func_, std::forward<impl_arg_refs_t>(in_and_out_refs));

        //LOG_FIBRE(SERDES, "will write output of types ", typeid(TOutputs).name()...);

        // serialize all outputs TODO: ignore some
        TMetadata::TOutputEncoders::serialize(output, std::tuple_cat(out_arg_vals, out_ret_vals));
    }

    uint16_t get_hash() const final {
        // TODO: implement
        return 0;
    }
    // @brief Returns a JSON snippet that describes this function
    bool get_as_json(const char ** output, size_t* length) const final {
        if (output) *output = metadata_.json.c_str();
        if (length) *length = metadata_.json.size();
        return true;
    }

private:
    TFunc func_;
    TMetadata metadata_;
};

/*template<typename TFunc, TFunc& func, typename TMetadata>
LocalFunctionEndpoint<TFunc, TMetadata> make_local_function_endpoint0(const TMetadata metadata) {
    static const auto json = FunctionJSONAssembler<TMetadata>::get_as_json(metadata);
    using ret_t = LocalFunctionEndpoint<TFunc, TMetadata>;
    return ret_t(func, json.c_str(), decltype(json)::size());
};*/

template<typename TFunc, typename TMetadata>
LocalFunctionEndpoint<TFunc&, TMetadata> make_local_function_endpoint(TFunc& func, TMetadata&& metadata) {
    using ret_t = LocalFunctionEndpoint<TFunc&, TMetadata>;
    return ret_t(std::forward<TFunc>(func), std::forward<TMetadata>(metadata));
};


template<typename TFunc, typename TMetadata>
LocalFunctionEndpoint<TFunc, TMetadata> make_local_function_endpoint(TFunc&& func, TMetadata&& metadata) {
    using ret_t = LocalFunctionEndpoint<TFunc, TMetadata>;
    return ret_t(std::forward<TFunc>(func), std::forward<TMetadata>(metadata));
};

}

#endif // __FIBRE_LOCAL_ENDPOINT
