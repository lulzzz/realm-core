/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <algorithm>

#include <realm/array.hpp>
#include <realm/bin_blob.hpp>

namespace realm {

class BigBlob : public Array {
public:
    explicit BigBlob(const BinBlob& b)
        : Array(b.get_alloc())
    {
        init_from_ref(b.get_ref());
    }

    explicit BigBlob(Allocator& alloc)
        : Array(alloc)
    {
        // Create new array with context flag set
        create(type_HasRefs, true); // Throws
    }

    size_t blob_size()
    {
        size_t total_size = 0;
        for (size_t i = 0; i < size(); ++i) {
            char* header = m_alloc.translate(Array::get_as_ref(i));
            total_size += Array::get_size_from_header(header);
        }
        return total_size;
    }

#ifdef REALM_DEBUG
    void verify()
    {
        REALM_ASSERT(has_refs());
        for (size_t i = 0; i < size(); ++i) {
            ref_type blob_ref = Array::get_as_ref(i);
            REALM_ASSERT(blob_ref != 0);
            BinBlob blob(m_alloc);
            blob.init_from_ref(blob_ref);
            blob.verify();
        }
    }
#endif

    BinaryData get_at(size_t& pos) const noexcept;
    ref_type replace(const char* data, size_t data_size);
};
}

using namespace realm;

BinaryData BigBlob::get_at(size_t& pos) const noexcept
{
    size_t offset = pos;
    size_t ndx = 0;
    size_t current_size = DatabaseElement::get_size_from_header(m_alloc.translate(Array::get_as_ref(ndx)));

    // Find the blob to start from
    while (offset >= current_size) {
        ndx++;
        if (ndx >= size()) {
            pos = 0;
            return {};
        }
        offset -= current_size;
        current_size = DatabaseElement::get_size_from_header(m_alloc.translate(Array::get_as_ref(ndx)));
    }

    BinBlob blob(m_alloc);
    blob.init_from_ref(Array::get_as_ref(ndx));
    ndx++;
    size_t sz = current_size - offset;

    // Check if we are at last blob
    pos = (ndx >= size()) ? 0 : pos + sz;

    return {blob.get(offset), sz};
}

ref_type BigBlob::replace(const char* data, size_t data_size)
{
    // We might have room for more data in the last node
    BinBlob lastNode(m_alloc);
    lastNode.init_from_ref(get_as_ref(size() - 1));
    lastNode.set_parent(this, size() - 1);

    size_t space_left = BinBlob::max_binary_size - lastNode.size();
    size_t size_to_copy = std::min(space_left, data_size);
    lastNode.add(data, size_to_copy);
    data_size -= space_left;
    data += space_left;

    while (data_size) {
        // Create new nodes as required
        size_to_copy = std::min(size_t(BinBlob::max_binary_size), data_size);
        BinBlob new_blob(m_alloc);
        new_blob.create(); // Throws

        // Copy data
        ref_type ref = new_blob.add(data, size_to_copy);
        // Add new node in hosting node
        Array::add(ref);

        data_size -= size_to_copy;
        data += size_to_copy;
    }
    return get_ref();
}

BinaryData BinBlob::get_at(size_t& pos) const noexcept
{
    size_t offset = pos;
    if (get_context_flag()) {
        BigBlob big(*this);
        return big.get_at(pos);
    }
    else {
        // All data is in this array
        pos = 0;
        if (offset < size()) {
            return {get(offset), size() - offset};
        }
        else {
            return {};
        }
    }
}

ref_type BinBlob::replace(size_t begin, size_t end, const char* data, size_t data_size, bool add_zero_term)
{
    REALM_ASSERT_3(begin, <=, end);
    REALM_ASSERT_3(end, <=, m_size);
    REALM_ASSERT(data_size == 0 || data);

    // The context flag indicates if the array contains references to blobs
    // holding the actual data.
    if (get_context_flag()) {
        REALM_ASSERT(begin == 0 && end == 0); // For the time being, only support append

        BigBlob big(*this);
        return big.replace(data, data_size);
    }
    else {
        size_t remove_size = end - begin;
        size_t add_size = add_zero_term ? data_size + 1 : data_size;
        size_t new_size = m_size - remove_size + add_size;

        // If size of BinaryData is below 'max_binary_size', the data is stored directly
        // in a single ArrayBlob. If more space is needed, the root blob will just contain
        // references to child blobs holding the actual data. Context flag will indicate
        // if blob is split.
        if (new_size > max_binary_size) {
            BigBlob new_root(m_alloc);

            // Add current node to the new root
            new_root.add(get_ref());
            return new_root.replace(data, data_size);
        }

        copy_on_write(); // Throws

        // Reallocate if needed - also updates header
        alloc(new_size, 1); // Throws

        char* modify_begin = m_data + begin;

        // Resize previous space to fit new data
        // (not needed if we append to end)
        if (begin != m_size) {
            const char* old_begin = m_data + end;
            const char* old_end = m_data + m_size;
            if (remove_size < add_size) { // expand gap
                char* new_end = m_data + new_size;
                std::copy_backward(old_begin, old_end, new_end);
            }
            else if (add_size < remove_size) { // shrink gap
                char* new_begin = modify_begin + add_size;
                std::copy(old_begin, old_end, new_begin);
            }
        }

        // Insert the data
        modify_begin = std::copy(data, data + data_size, modify_begin);
        if (add_zero_term)
            *modify_begin = 0;

        m_size = new_size;
    }
    return get_ref();
}

#ifdef REALM_DEBUG // LCOV_EXCL_START ignore debug functions

size_t BinBlob::blob_size() const noexcept
{
    if (get_context_flag()) {
        BigBlob bb(*this);
        return bb.blob_size();
    }
    else {
        return size();
    }
}

void BinBlob::verify() const
{
    if (get_context_flag()) {
        BigBlob bb(*this);
        bb.verify();
    }
    else {
        REALM_ASSERT(!has_refs());
    }
}

void BinBlob::to_dot(std::ostream& out, StringData title) const
{
    ref_type ref = get_ref();

    if (title.size() != 0) {
        out << "subgraph cluster_" << ref << " {" << std::endl;
        out << " label = \"" << title << "\";" << std::endl;
        out << " color = white;" << std::endl;
    }

    out << "n" << std::hex << ref << std::dec << "[shape=none,label=<";
    out << "<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\"><TR>" << std::endl;

    // Header
    out << "<TD BGCOLOR=\"lightgrey\"><FONT POINT-SIZE=\"7\"> ";
    out << "0x" << std::hex << ref << std::dec << "<BR/>";
    out << "</FONT></TD>" << std::endl;

    // Values
    out << "<TD>";
    out << blob_size() << " bytes"; // TODO: write content
    out << "</TD>" << std::endl;

    out << "</TR></TABLE>>];" << std::endl;

    if (title.size() != 0)
        out << "}" << std::endl;

    to_dot_parent_edge(out);
}

#endif // LCOV_EXCL_STOP ignore debug functions