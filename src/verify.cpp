#include <bitcoin/verify.hpp>

#include <bitcoin/block.hpp>
#include <bitcoin/dialect.hpp>
#include <bitcoin/constants.hpp>
#include <bitcoin/transaction.hpp>
#include <bitcoin/storage/storage.hpp>
#include <bitcoin/error.hpp>
#include <bitcoin/util/assert.hpp>
#include <bitcoin/util/logger.hpp>
#include <bitcoin/util/postbind.hpp>
#include <bitcoin/util/clock.hpp>

namespace libbitcoin {

using std::placeholders::_1;
using std::placeholders::_2;

constexpr size_t max_block_size = 1000000;
constexpr size_t max_block_script_operations = max_block_size / 50;

verify_block::verify_block(dialect_ptr dialect, 
    size_t depth, const message::block& current_block)
  : dialect_(dialect), depth_(depth), current_block_(current_block)
{
    clock_.reset(new clock);
}

bool verify_block::check()
{
    if (!check_block())
        return false;
    if (!accept_block())
        return false;
    return true;
}

bool verify_block::check_block()
{
    // CheckBlock()
    // These are checks that are independent of context
    // that can be verified before saving an orphan block
    // ...

    // Size limits
    if (current_block_.transactions.empty() || 
        current_block_.transactions.size() > max_block_size ||
        dialect_->to_network(current_block_, false).size() > max_block_size)
    {
        return false;
    }

    const hash_digest current_block_hash = hash_block_header(current_block_);
    if (!check_proof_of_work(current_block_hash, current_block_.bits))
        return false;

    const ptime block_time = 
            boost::posix_time::from_time_t(current_block_.timestamp);
    if (block_time > clock_->get_time() + hours(2))
        return false;

    if (!is_coinbase(current_block_.transactions[0]))
        return false;
    for (size_t i = 1; i < current_block_.transactions.size(); ++i)
    {
        const message::transaction& tx = current_block_.transactions[i];
        if (is_coinbase(tx))
            return false;
    }

    for (message::transaction tx: current_block_.transactions)
        if (!check_transaction(tx))
            return false;

    // Check that it's not full of nonstandard transactions
    if (number_script_operations() > max_block_script_operations)
        return false;

    if (current_block_.merkle_root != 
            generate_merkle_root(current_block_.transactions))
        return false;

    return true;
}

bool verify_block::check_proof_of_work(hash_digest block_hash, uint32_t bits)
{
    big_number target;
    target.set_compact(bits);

    if (target <= 0 || target > max_target())
        return false;
    
    big_number our_value;
    our_value.set_hash(block_hash);
    if (our_value > target)
        return false;

    return true;
}

bool verify_block::check_transaction(const message::transaction& tx)
{
    if (tx.inputs.empty() || tx.outputs.empty())
        return false;

    // Maybe not needed since we try to serialise block in CheckBlock()
    //if (dialect_->to_network(tx, false).size() > max_block_size)
    //    return false;

    // Check for negative or overflow output values
    uint64_t total_output_value = 0;
    for (message::transaction_output output: tx.outputs)
    {
        if (output.value > max_money())
            return false;
        total_output_value += output.value;
        if (total_output_value > max_money())
            return false;
    }

    if (is_coinbase(tx))
    {
        const script& coinbase_script = tx.inputs[0].input_script;
        size_t coinbase_script_size = save_script(coinbase_script).size();
        if (coinbase_script_size < 2 || coinbase_script_size > 100)
            return false;
    }
    else
    {
        for (message::transaction_input input: tx.inputs)
            if (previous_output_is_null(input))
                return false;
    }

    return true;
}

size_t verify_block::number_script_operations()
{
    size_t total_operations = 0;
    for (message::transaction tx: current_block_.transactions)
    {
        for (message::transaction_input input: tx.inputs)
            total_operations += input.input_script.operations().size();
        for (message::transaction_output output: tx.outputs)
            total_operations += output.output_script.operations().size();
    }
    return total_operations;
}

bool verify_block::accept_block()
{
    if (current_block_.bits != work_required())
        return false;
    if (current_block_.timestamp <= median_time_past())
        return false;
    // Txs should be final when included in a block
    // Do lesser check here since lock_time is currently unused in bitcoin
    for (const message::transaction& tx: current_block_.transactions)
        BITCOIN_ASSERT(tx.locktime == 0);
    if (!passes_checkpoints())
        return false;
    // AddToBlockIndex checks
    // network_->relay_inventory(...);
    return true;
}

uint32_t verify_block::work_required()
{
    if (depth_ == 0)
        return max_bits;
    else if (depth_ % readjustment_interval != 0)
        return previous_block_bits();

    uint64_t actual = actual_timespan(readjustment_interval);
    actual = range_constraint(actual, target_timespan / 4, target_timespan * 4);

    big_number retarget;
    retarget.set_compact(previous_block_bits());
    retarget *= actual;
    retarget /= target_timespan;

    if (retarget > max_target())
        retarget = max_target();

    return retarget.get_compact();
}

bool verify_block::passes_checkpoints()
{
    const hash_digest block_hash = hash_block_header(current_block_);

    if (depth_ == 11111 && block_hash !=
            hash_digest{0x00, 0x00, 0x00, 0x00, 0x69, 0xe2, 0x44, 0xf7, 
                        0x3d, 0x78, 0xe8, 0xfd, 0x29, 0xba, 0x2f, 0xd2, 
                        0xed, 0x61, 0x8b, 0xd6, 0xfa, 0x2e, 0xe9, 0x25, 
                        0x59, 0xf5, 0x42, 0xfd, 0xb2, 0x6e, 0x7c, 0x1d})
        return false;

    if (depth_ ==  33333 && block_hash !=
            hash_digest{0x00, 0x00, 0x00, 0x00, 0x2d, 0xd5, 0x58, 0x8a, 
                        0x74, 0x78, 0x4e, 0xaa, 0x7a, 0xb0, 0x50, 0x7a, 
                        0x18, 0xad, 0x16, 0xa2, 0x36, 0xe7, 0xb1, 0xce, 
                        0x69, 0xf0, 0x0d, 0x7d, 0xdf, 0xb5, 0xd0, 0xa6})
        return false;

    if (depth_ ==  68555 && block_hash !=
            hash_digest{0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x1b, 0x49, 
                        0x03, 0x55, 0x0a, 0x0b, 0x96, 0xe9, 0xa9, 0x40, 
                        0x5c, 0x8a, 0x95, 0xf3, 0x87, 0x16, 0x2e, 0x49, 
                        0x44, 0xe8, 0xd9, 0xfb, 0xe5, 0x01, 0xcd, 0x6a})
        return false;

    if (depth_ ==  70567 && block_hash !=
            hash_digest{0x00, 0x00, 0x00, 0x00, 0x00, 0x6a, 0x49, 0xb1, 
                        0x4b, 0xcf, 0x27, 0x46, 0x20, 0x68, 0xf1, 0x26, 
                        0x4c, 0x96, 0x1f, 0x11, 0xfa, 0x2e, 0x0e, 0xdd, 
                        0xd2, 0xbe, 0x07, 0x91, 0xe1, 0xd4, 0x12, 0x4a})
        return false;

    if (depth_ ==  74000 && block_hash !=
            hash_digest{0x00, 0x00, 0x00, 0x00, 0x00, 0x57, 0x39, 0x93, 
                        0xa3, 0xc9, 0xe4, 0x1c, 0xe3, 0x44, 0x71, 0xc0, 
                        0x79, 0xdc, 0xf5, 0xf5, 0x2a, 0x0e, 0x82, 0x4a, 
                        0x81, 0xe7, 0xf9, 0x53, 0xb8, 0x66, 0x1a, 0x20})
        return false;

    if (depth_ == 105000 && block_hash !=
            hash_digest{0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x91, 0xce, 
                        0x28, 0x02, 0x7f, 0xae, 0xa3, 0x20, 0xc8, 0xd2, 
                        0xb0, 0x54, 0xb2, 0xe0, 0xfe, 0x44, 0xa7, 0x73, 
                        0xf3, 0xee, 0xfb, 0x15, 0x1d, 0x6b, 0xdc, 0x97})
        return false;

    if (depth_ == 118000 && block_hash !=
            hash_digest{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x77, 0x4a, 
                        0x7f, 0x8a, 0x7a, 0x12, 0xdc, 0x90, 0x6d, 0xdb, 
                        0x9e, 0x17, 0xe7, 0x5d, 0x68, 0x4f, 0x15, 0xe0, 
                        0x0f, 0x87, 0x67, 0xf9, 0xe8, 0xf3, 0x65, 0x53})
        return false;

    if (depth_ == 134444 && block_hash !=
            hash_digest{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0xb1, 
                        0x2f, 0xfd, 0x4c, 0xd3, 0x15, 0xcd, 0x34, 0xff, 
                        0xd4, 0xa5, 0x94, 0xf4, 0x30, 0xac, 0x81, 0x4c, 
                        0x91, 0x18, 0x4a, 0x0d, 0x42, 0xd2, 0xb0, 0xfe})
        return false;

    return true;
}

} // libbitcoin

