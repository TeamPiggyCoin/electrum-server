#include <system_error>
#include <boost/logic/tribool.hpp>

#include <bitcoin/bitcoin.hpp>
using namespace libbitcoin;

#include <boost/python.hpp>
namespace python = boost::python;

#include "/home/genjix/python-bitcoin/src/primitive.h"

namespace ph = std::placeholders;

struct info_unit
{
    typedef std::vector<std::string> string_list;

    // block_hash == null_hash if mempool tx
    hash_digest tx_hash, block_hash;
    string_list inputs, outputs;
    size_t index;
    uint64_t value;
    size_t height;
    uint64_t timestamp;
    bool is_input;
    // set to empty if unused
    data_chunk raw_output_script;
};

typedef std::shared_ptr<info_unit> info_unit_ptr;

bool inputs_all_loaded(info_unit::string_list inputs)
{
    for (const auto& input: inputs)
        if (input.empty())
            return false;
    return true;
}

struct payment_entry
{
    bool is_loaded()
    {
        if (!loaded_output)
            return false;
        if (input_exists && !loaded_input)
            return false;
        return true;
    }

    message::output_point outpoint;
    info_unit_ptr loaded_output;
    // indeterminate until we know whether this output has a spend
    boost::tribool input_exists;
    message::input_point inpoint;
    // Ignore if input does not exist
    info_unit_ptr loaded_input;
};

typedef std::shared_ptr<payment_entry> payment_entry_ptr;

typedef std::vector<payment_entry_ptr> statement_entry;

typedef std::function<
    void (const std::error_code&, const statement_entry&)> finish_handler;

class query_history
  : public std::enable_shared_from_this<query_history>
{
public:
    query_history(async_service& service,
        blockchain_ptr chain, transaction_pool_ptr txpool)
      : strand_(service.get_service()), chain_(chain),
        txpool_(txpool), stopped_(false)
    {
    }

    void start(const std::string& address, finish_handler handle_finish)
    {
        address_.set_encoded(address);
        handle_finish_ = handle_finish;
        chain_->fetch_outputs(address_,
            strand_.wrap(std::bind(&query_history::start_loading,
                shared_from_this(), ph::_1, ph::_2)));
    }

private:
    // Not thread-safe
    void stop()
    {
        BITCOIN_ASSERT(stopped_ == false);
        stopped_ = true;
    }

    bool stop_on_error(const std::error_code& ec)
    {
        if (ec)
        {
            handle_finish_(ec, statement_entry());
            stop();
        }
        return stopped_;
    }

    void start_loading(const std::error_code& ec,
        const message::output_point_list& outpoints)
    {
        if (stop_on_error(ec))
            return;
        for (auto outpoint: outpoints)
        {
            auto entry = std::make_shared<payment_entry>();
            entry->outpoint = outpoint;
            statement_.push_back(entry);
            chain_->fetch_spend(outpoint,
                strand_.wrap(std::bind(&query_history::load_spend,
                    shared_from_this(), ph::_1, ph::_2, entry)));
            load_tx_info(outpoint, entry, false);
        }
    }

    void load_spend(const std::error_code& ec,
        const message::input_point inpoint, payment_entry_ptr entry)
    {
        // Need a custom self.stop_on_error(...) as a missing spend
        // is not an error in this case.
        if (ec && ec != error::unspent_output)
            stop_on_error(ec);
        if (stopped_)
            return;

        if (ec == error::unspent_output)
        {
            // This particular entry.output_point
            // has not been spent yet
            entry->input_exists = false;
            finish_if_done();
        }
        else
        {
            // Has been spent
            entry->input_exists = true;
            entry->inpoint = inpoint;
            load_tx_info(inpoint, entry, true);
        }
    }

    void finish_if_done()
    {
        for (auto entry: statement_)
            if (!entry->is_loaded())
                return;
        // Finish up
        for (auto entry: statement_)
        {
            if (entry->input_exists)
            {
                BITCOIN_ASSERT(entry->loaded_input && entry->loaded_output);
                // value of the input is simply the inverse of
                // the corresponding output
                entry->loaded_input->value = -entry->loaded_output->value;
                // Unspent outputs have a raw_output_script field
                // Blank this field as it isn't used
                entry->loaded_output->raw_output_script.clear();
            }
            else
            {
                // Unspent outputs have a raw_output_script field
            }
        }
        handle_finish_(std::error_code(), statement_);
        stop();
    }

    template <typename Point>
    void load_tx_info(const Point& point, payment_entry_ptr entry,
        bool is_input)
    {
        auto info = std::make_shared<info_unit>();
        info->tx_hash = point.hash;
        info->index = point.index;
        info->is_input = is_input;
        // Before loading the transaction, Stratum requires the hash
        // of the parent block, so we load the block depth and then
        // fetch the block header and hash it.
        chain_->fetch_transaction_index(point.hash,
            strand_.wrap(std::bind(&query_history::tx_index,
                shared_from_this(), ph::_1, ph::_2, ph::_3, entry, info)));
    }

    void tx_index(const std::error_code& ec, size_t block_depth,
        size_t offset, payment_entry_ptr entry, info_unit_ptr info)
    {
        if (stop_on_error(ec))
            return;
        info->height = block_depth;
        // And now for the block hash
        chain_->fetch_block_header(block_depth,
            strand_.wrap(std::bind(&query_history::block_header,
                shared_from_this(), ph::_1, ph::_2, entry, info)));
    }

    void block_header(const std::error_code& ec,
        const message::block blk_head,
        payment_entry_ptr entry, info_unit_ptr info)
    {
        if (stop_on_error(ec))
            return;
        info->timestamp = blk_head.timestamp;
        info->block_hash = hash_block_header(blk_head);
        // Now load the actual main transaction for this input or output
        chain_->fetch_transaction(info->tx_hash,
            strand_.wrap(std::bind(&query_history::load_chain_tx,
                shared_from_this(), ph::_1, ph::_2, entry, info)));
    }

    void load_tx(const message::transaction& tx, info_unit_ptr info)
    {
        // List of output addresses
        for (const auto& tx_out: tx.outputs)
        {
            payment_address addr;
            // Attempt to extract address from output script
            if (extract(addr, tx_out.output_script))
                info->outputs.push_back(addr.encoded());
            else
                info->outputs.push_back("Unknown");
        }
        // For the inputs, we need the originator address which has to
        // be looked up in the blockchain.
        // Create list of empty strings and then populate it.
        // Loading has finished when list is no longer all empty strings.
        for (const auto& tx_in: tx.inputs)
            info->inputs.push_back("");
        // If this transaction was loaded for an input, then we already
        // have a source address for at least one input.
        if (info->is_input)
        {
            BITCOIN_ASSERT(info->index < info->inputs.size());
            info->inputs[info->index] = address_.encoded();
        }
    }

    void load_chain_tx(const std::error_code& ec,
        const message::transaction& tx,
        payment_entry_ptr entry, info_unit_ptr info)
    {
        if (stop_on_error(ec))
            return;
        load_tx(tx, info);
        if (!info->is_input)
        {
            BITCOIN_ASSERT(info->index < tx.outputs.size());
            const auto& our_output = tx.outputs[info->index];
            info->value = our_output.value;
            // Save serialised output script in case this output is unspent
            info->raw_output_script = save_script(our_output.output_script);
        }
        // If all the inputs are loaded
        if (inputs_all_loaded(info->inputs))
        {
            // We are the sole input
            BITCOIN_ASSERT(info->is_input);
            entry->loaded_input = info;
            finish_if_done();
        }

        // *********
        // fetch_input_txs

        // Load the previous_output for every input so we can get
        // the output address
        for (size_t input_index = 0;
            input_index < tx.inputs.size(); ++input_index)
        {
            if (info->is_input && info->index == input_index)
                continue;
            const auto& prevout = tx.inputs[input_index].previous_output;
            chain_->fetch_transaction(prevout.hash,
                strand_.wrap(std::bind(&query_history::load_input_chain_tx,
                    shared_from_this(), ph::_1, ph::_2, prevout.index,
                        entry, info, input_index)));
        }
    }

    void load_input_tx(const message::transaction& tx, size_t output_index,
        info_unit_ptr info, size_t input_index)
    {
        // For our input, we load the previous tx so we can get the
        // corresponding output.
        // We need the output to extract the address.
        BITCOIN_ASSERT(output_index < tx.outputs.size());
        const auto& out_script = tx.outputs[output_index].output_script;
        payment_address addr;
        BITCOIN_ASSERT(input_index < info->inputs.size());
        if (extract(addr, out_script))
            info->inputs[input_index] = addr.encoded();
        else
            info->inputs[input_index] = "Unknown";
    }

    void load_input_chain_tx(const std::error_code& ec,
        const message::transaction& tx, size_t output_index,
        payment_entry_ptr entry, info_unit_ptr info, size_t input_index)
    {
        if (stop_on_error(ec))
            return;
        load_input_tx(tx, output_index, info, input_index);
        // If all the inputs are loaded, then we have finished loading
        // the info for this input-output entry pair
        if (inputs_all_loaded(info->inputs))
        {
            if (info->is_input)
                entry->loaded_input = info;
            else
                entry->loaded_output = info;
        }
        finish_if_done();
    }

    io_service::strand strand_;

    blockchain_ptr chain_;
    transaction_pool_ptr txpool_;
    bool stopped_;

    statement_entry statement_;
    payment_address address_;
    finish_handler handle_finish_;
};

typedef std::shared_ptr<query_history> query_history_ptr;

void write_xputs_strings(std::stringstream& ss, const char* xput_name,
    const info_unit::string_list& xputs)
{
    ss << '"' << xput_name << "\": [";
    for (auto it = xputs.begin(); it != xputs.end(); ++it)
    {
        if (it != xputs.begin())
            ss << ",";
        ss << '"' << *it << '"';
    }
    ss << "]";
}

void write_info(std::string& json, info_unit_ptr info)
{
    std::stringstream ss;
    ss << "{"
        << "\"tx_hash\": \"" << pretty_hex(info->tx_hash) << "\","
        << "\"block_hash\": \"" << pretty_hex(info->block_hash) << "\","
        << "\"index\": " << info->index << ","
        << "\"value\": " << info->value << ","
        << "\"height\": " << info->height << ","
        << "\"timestamp\": " << info->timestamp << ","
        << "\"is_input\": " << info->is_input << ",";
    write_xputs_strings(ss, "inputs", info->inputs);
    ss << ",";
    write_xputs_strings(ss, "outputs", info->outputs);
    if (!info->raw_output_script.empty())
        ss << ","
            << "\"raw_output_script\": \""
            << pretty_hex(info->raw_output_script) << "\"";
    ss << "}";
    json += ss.str();
}

void keep_query_alive_proxy(const std::error_code& ec,
    const statement_entry& statement,
    python::object handle_finish, query_history_ptr history)
{
    std::string json = "[";
    for (auto it = statement.begin(); it != statement.end(); ++it)
    {
        if (it != statement.begin())
            json += ",";
        auto entry = *it;
        BITCOIN_ASSERT(entry->loaded_output);
        write_info(json, entry->loaded_output);
    }
    json += "]";
    pyfunction<const std::error_code&, const std::string&> f(handle_finish);
    f(ec, json);
}

void payment_history(async_service_ptr service, blockchain_ptr chain,
    transaction_pool_ptr txpool, const std::string& address,
    python::object handle_finish)
{
    query_history_ptr history =
        std::make_shared<query_history>(*service, chain, txpool);
    history->start(address,
        std::bind(keep_query_alive_proxy, ph::_1, ph::_2,
            handle_finish, history));
}

BOOST_PYTHON_MODULE(_history)
{
    using namespace boost::python;
    def("payment_history", payment_history);
}

