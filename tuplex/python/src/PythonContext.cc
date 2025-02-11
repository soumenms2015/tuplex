//--------------------------------------------------------------------------------------------------------------------//
//                                                                                                                    //
//                                      Tuplex: Blazing Fast Python Data Science                                      //
//                                                                                                                    //
//                                                                                                                    //
//  (c) 2017 - 2021, Tuplex team                                                                                      //
//  Created by Leonhard Spiegelberg first on 1/1/2021                                                                 //
//  License: Apache 2.0                                                                                               //
//--------------------------------------------------------------------------------------------------------------------//

#include <PythonContext.h>
#include <LocalEngine.h>
#include <Row.h>
#include "python3_sink.h"
#include <JSONUtils.h>
#include <limits>
#include <Signals.h>

// possible classes are
// int, float, str, list, tuple, dict
// @TODO: there is also a possibility to add numpy array support!!!


// General notes:
// Interacting with boost python/PyObjects
// ==> boost::python::handle transfers ownership to boost python. Use with caution! Use on newly constructed objects
// ==> boost::python::borrowed is a borrowed reference, safer to use.

namespace tuplex {

    DataSet& PythonContext::fastF64Parallelize(PyObject* listObj, const std::vector<std::string>& columns, bool upcast) {
        assert(listObj);
        assert(PyList_Check(listObj));


        assert(columns.size() <= 1); // up to 1 column!

        size_t numElements = PyList_GET_SIZE(listObj);

        Schema schema(Schema::MemoryLayout::ROW, python::Type::makeTupleType({python::Type::F64}));

        // check if empty?
        if(0 == numElements)
            return _context->fromPartitions(schema, std::vector<Partition*>(), columns);

        // create new partition on driver
        auto driver = _context->getDriver();

        std::vector<Partition*> partitions;
        Partition* partition = driver->allocWritablePartition(allocMinSize, schema, -1);
        int64_t* rawPtr = (int64_t*)partition->lockWriteRaw();
        *rawPtr = 0;
        double* ptr = (double*)(rawPtr + 1);
        size_t numBytesSerialized = 0;
        for(unsigned i = 0; i < numElements; ++i) {
            auto obj = PyList_GET_ITEM(listObj, i);

            // check capacity and realloc if necessary get a new partition
            if(partition->capacity() < numBytesSerialized + sizeof(double)) {
                partition->unlockWrite();
                partitions.push_back(partition);
                partition = driver->allocWritablePartition(std::max(sizeof(double), allocMinSize), schema, -1);
                rawPtr = (int64_t*)partition->lockWriteRaw();
                *rawPtr = 0;
                ptr = (double*)(rawPtr + 1);
                numBytesSerialized = 0;
            }

            double val = 0.0;
            if(PyFloat_CheckExact(obj)) {
                val = PyFloat_AS_DOUBLE(obj);
            } else {
                if(upcast && (obj == Py_True || obj == Py_False || PyLong_CheckExact(obj))) {
                    if(obj == Py_True || obj == Py_False)
                        val = (double)(obj == Py_True);
                    else {
                        val = (double)PyLong_AsLongLong(obj);
                        if(PyErr_Occurred()) { // too large integer?
                            PyErr_Clear();
                            _badParallelizeObjects.emplace_back(std::make_tuple(i, obj));
                            continue;
                        }
                    }

                } else {
                    _badParallelizeObjects.emplace_back(std::make_tuple(i, obj));
                    continue;
                }
            }

            *ptr = val;
            ptr++;
            *rawPtr = *rawPtr + 1;
            numBytesSerialized += sizeof(double);
        }

        partition->unlockWrite();
        partitions.push_back(partition);

        // create dataset from partitions.
        return _context->fromPartitions(schema, partitions, columns);
    }

    DataSet& PythonContext::fastI64Parallelize(PyObject* listObj, const std::vector<std::string>& columns, bool upcast) {
        assert(listObj);
        assert(PyList_Check(listObj));

        size_t numElements = PyList_GET_SIZE(listObj);

        Schema schema(Schema::MemoryLayout::ROW, python::Type::makeTupleType({python::Type::I64}));

        // check if empty?
        if(0 == numElements)
            return _context->fromPartitions(schema, std::vector<Partition*>(), columns);

        // create new partition on driver
        auto driver = _context->getDriver();

        std::vector<Partition*> partitions;
        Partition* partition = driver->allocWritablePartition(std::max(sizeof(int64_t), allocMinSize), schema, -1);
        int64_t* rawPtr = (int64_t*)partition->lockWriteRaw();
        *rawPtr = 0;
        int64_t* ptr = rawPtr + 1;
        size_t numBytesSerialized = 0;
        for(unsigned i = 0; i < numElements; ++i) {
            auto obj = PyList_GET_ITEM(listObj, i);

            // check capacity and realloc if necessary get a new partition
            if(partition->capacity() < numBytesSerialized + sizeof(int64_t)) {
                partition->unlockWrite();
                partitions.push_back(partition);
                partition = driver->allocWritablePartition(std::max(sizeof(int64_t), allocMinSize), schema, -1);
                rawPtr = (int64_t*)partition->lockWriteRaw();
                *rawPtr = 0;
                ptr = rawPtr + 1;
                numBytesSerialized = 0;
            }

            int64_t val = 0;
            if(PyLong_CheckExact(obj)) {
                val = PyLong_AsLongLong(obj);
                if(PyErr_Occurred()) { // too large integer?
                    PyErr_Clear();
                    _badParallelizeObjects.emplace_back(std::make_tuple(i, obj));
                    continue;
                }
            } else {
                // auto upcast?
                if(upcast && (obj == Py_True || obj == Py_False))
                    val = obj == Py_True;
                else {
                    _badParallelizeObjects.emplace_back(std::make_tuple(i, obj));
                    continue;
                }
            }

            *ptr = val;
            ptr++;
            *rawPtr = *rawPtr + 1;
            numBytesSerialized += sizeof(int64_t);
        }

        partition->unlockWrite();
        partitions.push_back(partition);

        // create dataset from partitions.
        return _context->fromPartitions(schema, partitions, columns);
    }

    DataSet& PythonContext::fastMixedSimpleTypeTupleTransfer(PyObject *listObj, const python::Type &majType,
                                                             const std::vector<std::string> &columns) {
        assert(listObj);
        assert(PyList_Check(listObj));
        assert(majType.isTupleType());

        size_t numElements = PyList_GET_SIZE(listObj);
        size_t numTupleElements = majType.parameters().size();
        assert(columns.empty() || numTupleElements == columns.size());

        // now create partitions super fast
        Schema schema(Schema::MemoryLayout::ROW, majType);

        // check if empty?
        if(0 == numElements)
            return _context->fromPartitions(schema, std::vector<Partition*>(), columns);

        // encode type of tuple quickly into string
        char *typeStr = new char[numTupleElements];
        bool varLenField = makeTypeStr(majType, typeStr);

        size_t baseRequiredBytes = (numTupleElements + varLenField) * sizeof(int64_t); // if there's a varlen field, then we need to store also the varlen size!

        // create new partition on driver
        auto driver = _context->getDriver();

        std::vector<Partition*> partitions;
        Partition* partition = driver->allocWritablePartition(allocMinSize, schema, -1);
        int64_t* rawPtr = (int64_t*)partition->lockWriteRaw();
        *rawPtr = 0;
        uint8_t* ptr = (uint8_t*)(rawPtr + 1);
        size_t numBytesSerialized = 0;
        for(unsigned i = 0; i < numElements; ++i) {
            auto obj = PyList_GET_ITEM(listObj, i);

            // needs to be tuple with correct size
            bool check = PyTuple_Check(obj);
            check = check ? PyTuple_Size(obj) == numTupleElements : false;
            if(check) {

                // it's a tuple with macthing size
                // first get how many bytes are required
                size_t requiredBytes = baseRequiredBytes;
                if(varLenField) {
                    for(int j = 0; j < numTupleElements; ++j)
                        if(typeStr[j] == 's')
                            requiredBytes += PyUnicode_GET_SIZE(PyTuple_GetItem(obj, j)) + 1; // +1 for '\0'
                }

                // get new partition if capacity exhausted
                if(partition->capacity() < numBytesSerialized + requiredBytes) {
                    partition->unlockWrite();
                    partitions.push_back(partition);
                    partition = driver->allocWritablePartition(std::max(allocMinSize, requiredBytes), schema, -1);
                    rawPtr = (int64_t*)partition->lockWriteRaw();
                    *rawPtr = 0;
                    ptr = (uint8_t*)(rawPtr + 1);
                    numBytesSerialized = 0;
                }

                auto rowStartPtr = ptr;
                size_t rowVarFieldSizes = 0;
                // serialize based on type str
                for(int j = 0; j < numTupleElements; ++j) {
                    auto el = PyTuple_GET_ITEM(obj, j);
                    switch(typeStr[j]) {
                        case 'b': {
                            if(!PyBool_Check(el))
                                goto bad_element;

                            *((int64_t*)(ptr)) = el == Py_True ? 1 : 0;
                            ptr += sizeof(int64_t);
                            break;
                        }
                        case 'i': {
                            if(!PyLong_CheckExact(el))
                                goto bad_element;

                            *((int64_t*)(ptr)) = PyLong_AsLongLong(el);
                            ptr += sizeof(int64_t);
                            break;
                        }
                        case 'f': {
                            if(!PyFloat_CheckExact(el))
                                goto bad_element;

                            *((double*)(ptr)) = PyFloat_AS_DOUBLE(el);
                            ptr += sizeof(int64_t);
                            break;
                        }
                        case 's': {
                            if(!PyUnicode_Check(el))
                                goto bad_element;

                            auto utf8ptr = PyUnicode_AsUTF8(el);
                            auto len = PyUnicode_GET_SIZE(el);

                            assert(len == strlen(utf8ptr));
                            size_t varFieldSize = len + 1; // + 1 for '\0' char!
                            size_t varLenOffset = (numTupleElements + 1 - j) * sizeof(int64_t) + rowVarFieldSizes; // 16 bytes offset
                            int64_t info_field = varLenOffset | (varFieldSize << 32);

                            *((int64_t*)(ptr)) = info_field;

                            // copy string contents
                            memcpy(ptr + varLenOffset, utf8ptr, len + 1); // +1 for 0 delimiter
                            ptr += sizeof(int64_t); // move to next field
                            rowVarFieldSizes += varFieldSize;

                            break;
                        }
                    }
                }

                // serialize var len field if required
                if(varLenField) {
                    // after fixed length fields comes total varlen info field
                    *((int64_t*)(ptr)) = rowVarFieldSizes;
                }

                // inc row counter + push bytes + update ptr
                *rawPtr = *rawPtr + 1;
                numBytesSerialized += requiredBytes;
                ptr = rowStartPtr + requiredBytes;
                continue;

                // special part when bad row encountered
            bad_element:
                ptr = rowStartPtr;
                _badParallelizeObjects.emplace_back(std::make_tuple(i, obj));
            } else {
                _badParallelizeObjects.emplace_back(std::make_tuple(i, obj));
            }

            // serialization code here is a little bit more complicated
            // 3 fields need to be serialized:
            // (1) is the field containing offset + varlength
            // (2) is the field containing total varlength
            // (3) is the actual string content (incl. '\0' delimiter)
        }

        partition->unlockWrite();
        partitions.push_back(partition);

        delete [] typeStr;

        // create dataset from partitions.
        return _context->fromPartitions(schema, partitions, columns);
    }

    DataSet& PythonContext::fastBoolParallelize(PyObject *listObj, const std::vector<std::string>& columns) {
        assert(listObj);
        assert(PyList_Check(listObj));

        size_t numElements = PyList_GET_SIZE(listObj);

        Schema schema(Schema::MemoryLayout::ROW, python::Type::makeTupleType({python::Type::BOOLEAN}));

        // check if empty?
        if(0 == numElements)
            return _context->fromPartitions(schema, std::vector<Partition*>(), columns);

        // create new partition on driver
        auto driver = _context->getDriver();

        std::vector<Partition*> partitions;
        Partition* partition = driver->allocWritablePartition(std::max(sizeof(int64_t), allocMinSize), schema, -1);
        int64_t* rawPtr = (int64_t*)partition->lockWriteRaw();
        *rawPtr = 0;
        int64_t* ptr = rawPtr + 1;
        size_t numBytesSerialized = 0;
        for(unsigned i = 0; i < numElements; ++i) {
            auto obj = PyList_GET_ITEM(listObj, i);


            // check capacity and realloc if necessary get a new partition
            if(partition->capacity() < numBytesSerialized + sizeof(int64_t)) {
                partition->unlockWrite();
                partitions.push_back(partition);
                partition = driver->allocWritablePartition(std::max(sizeof(int64_t), allocMinSize), schema, -1);
                rawPtr = (int64_t*)partition->lockWriteRaw();
                *rawPtr = 0;
                ptr = rawPtr + 1;
                numBytesSerialized = 0;
            }

            if(PyBool_Check(obj)) {
                *ptr = obj == Py_True ? 1 : 0;
                ptr++;
                *rawPtr = *rawPtr + 1;
                numBytesSerialized += sizeof(int64_t);
            } else {
                _badParallelizeObjects.emplace_back(std::make_tuple(i, obj));
            }
        }

        partition->unlockWrite();
        partitions.push_back(partition);

        // create dataset from partitions.
        return _context->fromPartitions(schema, partitions, columns);
    }

    DataSet& PythonContext::fastStrParallelize(PyObject* listObj, const std::vector<std::string>& columns) {
        assert(listObj);
        assert(PyList_Check(listObj));

        size_t numElements = PyList_GET_SIZE(listObj);

        Schema schema(Schema::MemoryLayout::ROW, python::Type::makeTupleType({python::Type::STRING}));

        // check if empty?
        if(0 == numElements)
            return _context->fromPartitions(schema, std::vector<Partition*>(), columns);

        // create new partition on driver
        auto driver = _context->getDriver();

        std::vector<Partition*> partitions;
        Partition* partition = driver->allocWritablePartition(allocMinSize, schema, -1);
        int64_t* rawPtr = (int64_t*)partition->lockWriteRaw();
        *rawPtr = 0;
        uint8_t* ptr = (uint8_t*)(rawPtr + 1);
        size_t numBytesSerialized = 0;
        for(unsigned i = 0; i < numElements; ++i) {
            auto obj = PyList_GET_ITEM(listObj, i);

            // serialization code here is a little bit more complicated
            // 3 fields need to be serialized:
            // (1) is the field containing offset + varlength
            // (2) is the field containing total varlength
            // (3) is the actual string content (incl. '\0' delimiter)
            if(PyUnicode_Check(obj)) {

                auto len = PyUnicode_GET_SIZE(obj);

                auto utf8ptr = PyUnicode_AsUTF8(obj);

                size_t requiredBytes = sizeof(int64_t) * 2 + len + 1;

                // check capacity and realloc if necessary get a new partition
                if(partition->capacity() < numBytesSerialized + requiredBytes) {
                    partition->unlockWrite();
                    partitions.push_back(partition);
                    partition = driver->allocWritablePartition(std::max(allocMinSize, requiredBytes), schema, -1);
                    rawPtr = (int64_t*)partition->lockWriteRaw();
                    *rawPtr = 0;
                    ptr = (uint8_t*)(rawPtr + 1);
                    numBytesSerialized = 0;
                }

                assert(len == strlen(utf8ptr));

                size_t varFieldSize = len + 1; // + 1 for '\0' char!
                size_t varLenOffset = 2 * sizeof(int64_t); // 16 bytes offset
                int64_t info_field = varLenOffset | (varFieldSize << 32);

                *((int64_t*)(ptr)) = info_field;
                // after fixed length fields comes total varlen info field
                *((int64_t*)(ptr + sizeof(int64_t))) = varFieldSize;
                // copy string contents
                memcpy(ptr + sizeof(int64_t) * 2, utf8ptr, len + 1); // +1 for 0 delimiter
                ptr += requiredBytes;
                *rawPtr = *rawPtr + 1;
                numBytesSerialized += requiredBytes;
            } else {
                _badParallelizeObjects.emplace_back(std::make_tuple(i, obj));
            }
        }

        partition->unlockWrite();
        partitions.push_back(partition);

        // create dataset from partitions.
        return _context->fromPartitions(schema, partitions, columns);
    }

    // Returns true if t1 can be considered a subtype of t2, specifically in the context of Option types
    // For example, t1=int, t2=Option[int] -> true
    // Similarly, t2=(int, none), t2=(Option[int], Option[int]) -> true
    bool isSubOptionType(python::Type t1, python::Type t2) {
        if(t1 == t2) return true; // same type -> return true
        if(t2.isOptionType() && (t1 == t2.getReturnType() || t1 == python::Type::NULLVALUE)) return true; // t2 is an option and t1 is a subtype
        if(t1.isTupleType() && t2.isTupleType() && t1.parameters().size() == t2.parameters().size()) {
            // if they are both tuples of the same size, recursively check each field to see whether they are subtypes
            for(int i=0; i<t2.parameters().size(); i++) {
                if(!isSubOptionType(t1.parameters()[i], t2.parameters()[i])) return false;
            }
            return true;
        }

        return false;
    }

    DataSet & PythonContext::parallelizeAnyType(boost::python::list &L, const python::Type &majType, const std::vector<std::string>& columns) {

        auto& logger = Logger::instance().logger("python");
        logger.info("using slow transfer to backend");

        // ref counting error has to occur somewhere here...

        // general slow version
        Schema schema(Schema::MemoryLayout::ROW, majType);

        // get list item
        auto listObj = L.ptr();

        auto numElements = PyList_Size(listObj);
        logger.debug("transferring " + std::to_string(numElements) + " elements. ");

        // decode tuple
        std::vector<Row> v;
        for (unsigned i = 0; i < numElements; ++i) {

            // because this a slow transfer loop, check explicitly for signals and free anything if there's something...
            // if(check_and_forward_signals(true)) {
            // check if interrupted, if so return!
            // Note: correct signal behavior should call whatever user function exists...
            if(check_interrupted()) {
                // do not clear signal yet! => leads to correct calling of python's signal handlers!
                logger.warn("slow transfer to backend interrupted.");

                // free items (decref)
                for(auto t : _badParallelizeObjects) {
                    Py_XDECREF(std::get<1>(t));
                }
                _badParallelizeObjects.clear();
                v.clear();

                return _context->makeError("interrupted transfer");
            }

            auto item = PyList_GET_ITEM(listObj, i);

            // cf. http://www.cse.psu.edu/~gxt29/papers/refcount.pdf
            Py_XINCREF(item);

            python::Type t = python::mapPythonClassToTuplexType(item);
            if(isSubOptionType(t, majType)) {
                // In this case, t is a subtype of the majority type; this accounts for the case where the majority type
                // is an option (e.g. majType=Option[int] should encompass both t=I64 and t=NULLVALUE).
                v.push_back(python::pythonToRow(item, majType));

            } else
                _badParallelizeObjects.emplace_back(std::make_tuple(i, item));
        }

        // serialize in main memory
        return _context->parallelize(v, columns);
    }

    DataSet& PythonContext::strDictParallelize(PyObject *listObj, const python::Type &rowType,
                                               const std::vector<std::string> &columns) {
        assert(listObj);
        assert(PyList_Check(listObj));

        const auto allocMinSize = 100;

        size_t numElements = PyList_GET_SIZE(listObj);

        assert(rowType.isTupleType()); // important!!!
        assert(rowType.parameters().size() == columns.size()); // also very important!!!
        Schema schema(Schema::MemoryLayout::ROW, rowType);

        // check if empty?
        if(0 == numElements)
            return _context->fromPartitions(schema, std::vector<Partition*>(), columns);

        // create new partition on driver
        auto driver = _context->getDriver();

        std::vector<Partition*> partitions;
        Partition* partition = driver->allocWritablePartition(allocMinSize, schema, -1);
        int64_t* rawPtr = (int64_t*)partition->lockWriteRaw();
        *rawPtr = 0;
        uint8_t* ptr = (uint8_t*)(rawPtr + 1);
        size_t numBytesSerialized = 0;
        for(unsigned i = 0; i < numElements; ++i) {
            auto obj = PyList_GET_ITEM(listObj, i);

            // check that it is a dict!
            if(PyDict_Check(obj)) {
                auto numDictElements = PyDict_Size(obj);

                // first check, do sizes match?
                if(numDictElements != rowType.parameters().size())
                    _badParallelizeObjects.emplace_back(std::make_tuple(i, obj));
                else {
                    // same number of elements.
                    // ==> need to get columns etc. out
                    bool good = true;
                    PyObject *tupleObj = PyTuple_New(rowType.parameters().size());
                    int j = 0;
                    for(const auto& c : columns) {
                        auto item = PyDict_GetItemString(obj, c.c_str());

                        // item is borrowed, reference. So incref!
                        // https://docs.python.org/3/c-api/dict.html#c.PyDict_GetItemString
                        Py_XINCREF(item);

                        if(!item) {
                            _badParallelizeObjects.emplace_back(std::make_tuple(i, obj));
                            good = false;
                            // set dummy
                            Py_XINCREF(Py_None);
                            PyTuple_SET_ITEM(tupleObj, j, Py_None);
                        } else
                            PyTuple_SET_ITEM(tupleObj, j, item);
                        ++j;
                    }

                    // check if all good still or there was an issue with a column...
                    if(!good)
                        continue;

                    // all the item are extracted into a tuple.
                    // ==> convert to row object & check type
                    Row row = python::pythonToRow(tupleObj);

                    // Py_XDECREF(tupleObj); // remove temporary tupleObject

                    if(row.getRowType() != rowType)
                        _badParallelizeObjects.emplace_back(std::make_tuple(i, obj));
                    else {
                        // write to partition

                        size_t requiredBytes = row.serializedLength();
                        // check capacity and realloc if necessary get a new partition
                        if(partition->capacity() < numBytesSerialized + allocMinSize) {
                            partition->unlockWrite();
                            partitions.push_back(partition);
                            partition = driver->allocWritablePartition(allocMinSize, schema, -1);
                            rawPtr = (int64_t*)partition->lockWriteRaw();
                            *rawPtr = 0;
                            ptr = (uint8_t*)(rawPtr + 1);
                            numBytesSerialized = 0;
                        }

                        row.serializeToMemory(ptr, partition->capacity());
                        ptr += requiredBytes;
                        *rawPtr = *rawPtr + 1;
                        numBytesSerialized += requiredBytes;
                    }
                }
            }
        }

        partition->unlockWrite();
        partitions.push_back(partition);

        // create dataset from partitions.
        return _context->fromPartitions(schema, partitions, columns);
    }

    PythonDataSet PythonContext::parallelize(boost::python::list L,
                                             boost::python::object cols,
                                             boost::python::object schema) {

        assert(_context);

        // clear bad parallelize cache
        _badParallelizeObjects.clear(); // <-- this holds all objects who don't comply with majority type

        auto& logger = Logger::instance().logger("python");
        auto columns = extractFromListOfStrings(cols.ptr(), "columns ");
        PythonDataSet pds;
        DataSet *ds = nullptr;
        python::Type majType; // the type assumed for the dataset
        auto autoUpcast = _context->getOptions().AUTO_UPCAST_NUMBERS();

        Timer timer;
        auto numElements = boost::python::len(L);
        std::stringstream ss;
        ss<<"transferring "<<numElements<<" elements to tuplex";
        logger.info(ss.str());


        // Transfer logic starts here
        // ---------------------------------------------------------------------

        // check if schema is not none, if so deduce
        auto schemaObj = schema.ptr(); assert(schemaObj);
        bool hasExplicitSchema = schemaObj != Py_None;
        python::Type explicitRowType;
        if(hasExplicitSchema) {
            majType = python::decodePythonSchema(schemaObj);
        } else
            majType = inferType(L);

        // special case: majType is a dict with strings as key, i.e. perform String Dict unpacking
        if((majType.isDictionaryType() && majType != python::Type::EMPTYDICT && majType != python::Type::GENERICDICT) && majType.keyType() == python::Type::STRING) {
            // automatic unpacking!
            // ==> first check if columns are defined, if not infer columns from sample!
            auto dictTypes = inferColumnsFromDictObjects(L, _context->getOptions().NORMALCASE_THRESHOLD());

            // are columns empty? ==> keys are new columns, create type out of that!
            if(columns.empty()) {
                for(auto keyval : dictTypes)
                    columns.push_back(keyval.first);
            }

            // create type based on columns
            std::vector<python::Type> types;
            for(const auto& c : columns) {
                auto it = dictTypes.find(c);
                if(it == dictTypes.end()) {
                    logger.warn("column " + c + " not found in sample. Assuming type Any for it.");
                    types.push_back(python::Type::PYOBJECT);
                }
                else
                    types.push_back(dictTypes[c]);
            }

            majType = python::Type::makeTupleType(types);

            // have to use special dict parallelize function here!
            ds = &strDictParallelize(L.ptr(), majType, columns);
        }
        // fast convert
        else if(majType == python::Type::BOOLEAN)
            ds = &fastBoolParallelize(L.ptr(), columns);
        else if(majType == python::Type::I64)
            ds = &fastI64Parallelize(L.ptr(), columns, autoUpcast);
        else if(majType == python::Type::F64)
            ds = &fastF64Parallelize(L.ptr(), columns, autoUpcast);
        else if(majType == python::Type::STRING)
            ds = &fastStrParallelize(L.ptr(), columns);
        else if(majType.isTupleType()) {
            // check whether it's a tuple consisting of simple types only, if so transfer super fast!
                if(python::tupleElementsHaveSimpleTypes(majType)) {

                // mixed simple types ==> can do faster transfer here!
                    ds = &fastMixedSimpleTypeTupleTransfer(L.ptr(), majType, columns);
                } else {
                    // general slow transfer...
               ds = &parallelizeAnyType(L, majType, columns);}
        } else if(majType.isDictionaryType() || majType == python::Type::GENERICDICT) {
            ds = &parallelizeAnyType(L, majType, columns);
        } else if(majType.isOptionType()) {
            // TODO: special case to fast conversion for the option types with fast underlying types
            ds = &parallelizeAnyType(L, majType, columns);
        } else if(majType == python::Type::NULLVALUE) {
            // TODO: special case to fast conversion for the option types with fast underlying types
            ds = &parallelizeAnyType(L, majType, columns);
        } else if(majType.isListType()) {
            ds = &parallelizeAnyType(L, majType, columns);
        } else if(majType == python::Type::PYOBJECT) {
            ds = &parallelizeAnyType(L, majType, columns);
        } else {
            std::string msg = "unsupported type '" + majType.desc() + "' found, could not transfer data to backend";
            Logger::instance().logger("python").error(msg);
            ds = &_context->makeError(msg);
        }

        // check if unknown type
        if(majType == python::Type::UNKNOWN)
            logger.error("unknown type detected as default type, can't process normal case");

        // else, transfer data under this type...
        logger.info("inferred default type is " + majType.desc());


        // success message only if dataset is not an error dataset
        if(!ds->isError()) {
            // compute size in memory
            size_t sizeInMemory = 0;
            for(auto p : ds->getPartitions())
                sizeInMemory += p->size();

            Logger::instance().logger("python").info("Data transfer to backend took "
            + std::to_string(timer.time()) + " seconds (materialized: " + sizeToMemString(sizeInMemory) + ")");
        }

        // warning about bad objects
        if(!_badParallelizeObjects.empty()) {
            // warn!
            logger.warn("Found " + pluralize(_badParallelizeObjects.size(), "row") + " not complying with inferred type " + majType.desc()
            + ", ignoring for now.");

            // @TODO: later save these rows as cloudpickled objects to a partition together with the row number.
            // they then need to be passed through the pure python pipeline & merged back if possible.

            // remove all..
            _badParallelizeObjects.clear();
        }

        // assign dataset to wrapper
        pds.wrap(ds);

        Logger::instance().logger("python").debug("wrapped dataset, returning it");

        Logger::instance().flushAll();

        return pds;
    }

    // This function returns true if there is an Option type that both t1, t2 can be classified as
    // If it returns true, it places the "super option" type into the parameter [super].
    // For example, t1=int, t2=None -> super = Option[int]
    // Similarly, t1=(int, none), t2=(none, int) -> super = (Option[int], Option[int])
    bool hasSuperOptionType(python::Type t1, python::Type t2, python::Type &super) {
        // same type
        if(t1 == t2) {
            super = t1;
            return true;
        }
        if(t1.isOptionType() && (t1.getReturnType() == t2 || python::Type::NULLVALUE == t2)) {
            super = t1;
            return true;
        }
        if(t2.isOptionType() && (t2.getReturnType() == t1 || python::Type::NULLVALUE == t1)) {
            super = t2;
            return true;
        }

        // one of them is null
        if(t1 == python::Type::NULLVALUE) {
            super = python::Type::makeOptionType(t2);
            return true;
        }
        if(t2 == python::Type::NULLVALUE) {
            super = python::Type::makeOptionType(t1);
            return true;
        }

        // both tuples, recurse
        if (t1.isTupleType() && t2.isTupleType() && t1.parameters().size() == t2.parameters().size()) {
            std::vector<python::Type> types(t1.parameters().size());
            for(int i=0; i<types.size(); i++) {
                if(!hasSuperOptionType(t1.parameters()[i], t2.parameters()[i], types[i])) return false;
            }
            super = python::Type::makeTupleType(types);
            return true;
        }

        return false;
    }

    python::Type buildRowTypeFromSamples(const std::map<python::Type, int> &colTypes, int numSamples, double threshold) {
        Logger::instance().logger("python").info("inferring type!");
        std::map<int, int> tupleLengthCounter; // count for each length of tuples how often it was seen in the sample

        // get majority type (--> i.e. hash aggregate!)
        int max = std::numeric_limits<int>::min();
        python::Type majType = python::Type::UNKNOWN;
        int maxTuple = std::numeric_limits<int>::min();
        python::Type majTupleType = python::Type::UNKNOWN; // we are willing to "optionize" each of the fields of this

        // Note: need to prefer bigger types over smaller ones!
        // ==> convert to tuples, then sort & fetch max!
        std::vector<std::tuple<python::Type, int>> types;

        for(const auto& it : colTypes)
            types.emplace_back(std::make_tuple(it.first, it.second));

        std::sort(types.begin(), types.end(), [](const std::tuple<python::Type, int>& lhs,
                const std::tuple<python::Type, int>& rhs) {
            return std::get<0>(rhs).isSubclass(std::get<0>(lhs));
        });

        for(const auto& entry : types) {
            const auto& type = std::get<0>(entry);
            auto frequency = std::get<1>(entry);
            if(frequency > max) {
                max = frequency;
                majType = type;
            }
            if(type.isTupleType() && frequency > maxTuple) {
                maxTuple = frequency;
                majTupleType = type;
            }
        }

        if(majTupleType.isTupleType()) {
            // check if we can optionize the tuple fields and make it the majority type
            python::Type superTuple = majTupleType;
            int num = 0; // the number of elements that will go under the new type
            for (const auto &it : colTypes) {
                // recurse on each of the fields
                if(hasSuperOptionType(it.first, superTuple, superTuple)) {
                    num += it.second;
                }
            }
            double fraction = (double)(num - colTypes.at(majTupleType))/(double)numSamples;
            if(num > max && fraction > 1-threshold && fraction < threshold) majType = superTuple;
        }

        // count number of none
        if(majType != python::Type::UNKNOWN && majType != python::Type::NULLVALUE && colTypes.count(python::Type::NULLVALUE)) {
            double noneFraction = (double)colTypes.at(python::Type::NULLVALUE)/(double)numSamples;
            if(noneFraction > 1 - threshold && noneFraction < threshold) {
                majType = python::Type::makeOptionType(majType);
            }
        }

        return majType;
    }

    python::Type PythonContext::inferType(const boost::python::list &L) const {
        // elements must be either simple objects, i.e. str/int/float
        // or tuples of simple objects
        // ==> no support for lists yet!!!
        // first of all start scanning elements and determine type of the partition where data is streamed to

        auto numSample = sampleSize(L);

        // new thing about tuplex is, that we allow for erroneous data => i.e. determine from sampling normal case
        std::map<python::Type, int> mTypes; // count for each type how often it was seen in the sample
        for(unsigned i = 0; i < numSample; ++i) {
            boost::python::object o = L[i];

            // describe using internal types
            python::Type t = python::mapPythonClassToTuplexType(o.ptr());

            if(mTypes.find(t) == mTypes.end())
                mTypes[t] = 1;
            else
                mTypes[t] += 1;
        }

        // be sure to also collapse types to supertypes if possible...
        if(mTypes.size() > 1)
            Logger::instance().logger("python").warn("more than one type in column found");

        return buildRowTypeFromSamples(mTypes, numSample, _context->getOptions().OPTIONAL_THRESHOLD());
    }

    std::unordered_map<std::string, python::Type> PythonContext::inferColumnsFromDictObjects(const boost::python::list &L, double normalThreshold) {
        using namespace std;

        auto& logger = Logger::instance().logger("python");

        auto numSample = sampleSize(L);

#warning "use here and for other sample based inferences a global infer method!"
        // count occurences of columns, decide on heuristic which are normal and which should become exceptions
        unordered_map<string, size_t> counts;
        unordered_map<string, vector<PyObject*>> cols;
        size_t num_dicts = 0;
        PyObject* listObj = L.ptr(); assert(listObj); assert(PyList_Check(listObj));
        for(int i = 0; i < numSample; ++i) {
            auto item = PyList_GET_ITEM(listObj, i);

            Py_INCREF(item); // borrowed reference?

            if(PyDict_Check(item)) {
                num_dicts++;

                // go through keys...
                PyObject *key = nullptr, *val = nullptr;
                Py_ssize_t pos = 0; // must be initialized to 0 to start iteration, however internal iterator variable. Don't use semantically.
                while(PyDict_Next(item, &pos, &key, &val)) {
                    // check if key is string
                    if(PyUnicode_Check(key)) {
                        auto skey = python::PyString_AsString(key);
                        auto it = counts.find(skey);
                        if(it == counts.end()) {
                            counts[skey] = 0;
                            cols[skey] = std::vector<PyObject*>();
                        }
                        counts[skey]++;
                        Py_XINCREF(val); // val is borrowed according to https://docs.python.org/3/c-api/dict.html#c.PyDict_Next
                        cols[skey].push_back(val);
                    }
                }
            }
        }

        // normal case decision time!
        vector<string> columns;
        for(const auto& keyval : counts) {
            // met threshold?
            if(keyval.second >= ceil(normalThreshold * num_dicts))
                columns.emplace_back(keyval.first);
        }

        // infer for each of the columns the most likely type!
        unordered_map<string, python::Type> m;
        for(const auto& c : columns) {
            PyObject* listColObj = PyList_New(cols[c].size());
            for(int i = 0; i < cols[c].size(); ++i) {
                Py_XINCREF(cols[c][i]);
                PyList_SET_ITEM(listColObj, i, cols[c][i]);
            }

            // hand-off to infer type function
            // ==> note: boost::python::handle transfers ownership!
            auto type = inferType(boost::python::list(boost::python::handle<>(listColObj)));

            m[c] = type;
        }


        // special case: no inference was possible ==> take as backup the first row as schema. warn message.
        if(m.empty()) {
            logger.warn("could not infer column names from sample according to threshold. Defaulting to schema defined by first row.");

            PyObject *item = nullptr;
            for(int i = 0; i < PyList_Size(listObj); ++i) {
                item = PyList_GET_ITEM(listObj, i);
                if(PyDict_Check(item)) {

                    // check that keys are all strings
                    auto keys = PyDict_Keys(item);
                    assert(PyList_Check(keys));
                    bool all_strs = true;
                    for(int j = 0; j < PyList_Size(keys); ++j)
                        if(PyList_GET_ITEM(keys, j)->ob_type != &PyUnicode_Type)
                            all_strs = false;
                    if(all_strs)
                        break;
                }
                item = nullptr;
            }

            if(!item || !PyDict_Check(item))
                throw std::runtime_error("type inference from dictionary objects failed. Please provide manually a schema.");

            // fetch all values and
            auto items = PyDict_Items(item);
            assert(PyList_Check(items));
            for(int j = 0; j < PyList_Size(items); ++j) {
                auto keyval = PyList_GET_ITEM(items, j);
                assert(PyTuple_Check(keyval));
                assert(PyTuple_Size(keyval) == 2);
                // just use directly the type...
                auto key = PyTuple_GET_ITEM(keyval, 0);
                auto val = PyTuple_GET_ITEM(keyval, 1);
                assert(PyUnicode_Check(key));
                m[python::PyString_AsString(key)] = python::mapPythonClassToTuplexType(val);
            }
        }

        // return map
        return m;
    }

    PythonDataSet PythonContext::csv(const std::string &pattern,
                                     boost::python::object cols,
                                     bool autodetect_header,
                                     bool header,
                                     const std::string& delimiter,
                                     const std::string& quotechar,
                                     boost::python::object null_values,
                                     boost::python::object type_hints) {
        assert(_context);

        // reset signals
        if(check_and_forward_signals(true))
            return makeError("job aborted via signal");

        PythonDataSet pds;

        //#ifndef NDEBUG
        //        using namespace std;
        //        cout<<"file pattern is: "<<pattern<<endl;
        //        cout<<"auto detect header: "<<boolToString(autodetect_header)<<endl;
        //        cout<<"header: "<<boolToString(header)<<endl;
        //        cout<<"delimiter: "<<delimiter<<endl;
        //        cout<<"quotechar: "<<quotechar<<endl;
        //#endif

        assert(quotechar.size() == 1);
        assert(delimiter.size() <= 1);

        assert(PyGILState_Check()); // make sure this thread holds the GIL!

        // extract columns (if not none)
        auto columns = extractFromListOfStrings(cols.ptr(), "columns ");
        auto null_value_strs = extractFromListOfStrings(null_values.ptr(), "null_values ");
        auto type_idx_hints_c = extractIndexBasedTypeHints(type_hints.ptr(), columns, "type_hints ");
        auto type_col_hints_c = extractColumnBasedTypeHints(type_hints.ptr(), columns, "type_hints ");

        python::unlockGIL();
        DataSet *ds = nullptr;
        std::string err_message = "";
        try {
            ds = &_context->csv(pattern, columns, autodetect_header ? option<bool>::none : option<bool>(header),
                                delimiter.empty() ? option<char>::none : option<char>(delimiter[0]),
                                quotechar[0], null_value_strs, type_idx_hints_c, type_col_hints_c);
        } catch(const std::exception& e) {
            err_message = e.what();
            Logger::instance().defaultLogger().error(err_message);
        } catch(...) {
            err_message = "unknown C++ exception occurred, please change type.";
            Logger::instance().defaultLogger().error(err_message);
        }

        python::lockGIL();

        // nullptr? then error dataset!
        if(!ds || !err_message.empty()) {
            Logger::instance().flushAll();
            assert(_context);
            ds = &_context->makeError(err_message);
        }
        pds.wrap(ds);
        Logger::instance().flushAll();
        return pds;
    }

    PythonDataSet PythonContext::text(const std::string &pattern, boost::python::object null_values ) {
        assert(_context);

        // reset signals
        if(check_and_forward_signals(true))
            return makeError("job aborted via signal");

        PythonDataSet pds;
        assert(PyGILState_Check()); // make sure this thread holds the GIL!
        auto null_value_strs = extractFromListOfStrings(null_values.ptr(), "null_values ");

        python::unlockGIL();
        DataSet *ds = nullptr;
        std::string err_message = "";
        try {
            ds = &_context->text(pattern, null_value_strs);
        } catch(const std::exception& e) {
            err_message = e.what();
            Logger::instance().defaultLogger().error(err_message);
        } catch(...) {
            err_message = "unknown C++ exception occurred, please change type.";
            Logger::instance().defaultLogger().error(err_message);
        }

        python::lockGIL();

        // nullptr? then error dataset!
        if(!ds || !err_message.empty()) {
            Logger::instance().flushAll();
            assert(_context);
            ds = &_context->makeError(err_message);
        }
        pds.wrap(ds);
        Logger::instance().flushAll();
        return pds;
    }

    ContextOptions updateOptionsWithDict(ContextOptions co, const std::string& options) {
        // convert json dictionary to C++ map
        auto m = jsonToMap(options);

        ContextOptions defaults = ContextOptions::defaults();

        auto& logger = Logger::instance().logger("python");

        // go through keyval pairs and check whether they exist in default
        for(auto keyval : m) {
            auto key = keyval.first;
            auto val = keyval.second;

            // check if key or key with tuplex. appended exists
            if(defaults.containsKey(key)) {
                co.set(key, val);
            } else if(defaults.containsKey("tuplex." + key)) {
                co.set("tuplex." + key, val);
            }
            else
                logger.warn("key '" + key + "' with value '" + val + "' is not a valid Tuplex option.");
        }

        return co;
    }

   // // running with another python version might lead to severe issues
   // // hence, perform check at context startup!
   // bool checkPythonVersion() {
   //    using namespace std;
   //    cout<<"PYTHON ABI: "<<PYTHON_ABI_STRING<<endl;
   //    cout<<"Compiled Python version: "<<PY_MAJOR_VERSION<<"."<<PY_MINOR_VERSION<<"."<<PY_MICRO_VERSION<<endl;
   //    cout<<"Retrieved Python version: "<<Py_GetVersion()<<endl;
   //    cout<<"Python home: "<<Py_GetPythonHome()<<endl;
   //    return true;
   // }

    PythonContext::PythonContext(const std::string& name,
                                 const std::string &runtimeLibraryPath,
                                 const std::string& options) {

        using namespace std;

        // checkPythonVersion();

        ContextOptions co = ContextOptions::defaults();

#warning "uncomment this code, because it will cause a deadlock sometimes... (Weird python output shit)"
        // init logging system here
        // @TODO: add as context option
        // note: this should come BEFORE any Logger::instance()... calls
        //Logger::init({std::make_shared<python3_sink_mt>()});

        if(runtimeLibraryPath.length() > 0)
            co.set("tuplex.runTimeLibrary", runtimeLibraryPath);

         co = updateOptionsWithDict(co, options);

        //#ifndef NDEBUG
        //        // print settings
        //        Logger::instance().defaultLogger().info("Tuplex configuration:");
        //        auto store = co.store();
        //        for(auto keyval : store) {
        //            Logger::instance().defaultLogger().info(keyval.first + "=" + keyval.second);
        //        }
        //#endif

        // testwise retrieve runtime path. This may be a critical error, hence throw PyException!
        python::unlockGIL();
        auto uri = co.RUNTIME_LIBRARY(true);
        python::lockGIL();
        if(uri == URI::INVALID) {
            throw PythonException("Could not find runtime library under " + co.get("tuplex.runTimeLibrary"));
        }

        // store explicitly uri in context options so no searching happens anymore
        Logger::instance().defaultLogger().debug("Using runtime library from  " + uri.toPath());
        co.set("tuplex.runTimeLibrary", uri.toPath());

        // for context creation release GIL
        assert(PyGILState_Check()); // make sure this thread holds the GIL!
        python::unlockGIL();
        _context = new Context(co);
        if(!name.empty())
            _context->setName(name);
        // restore GIL
        python::lockGIL();
        Logger::instance().flushAll();
    }


    PythonContext::~PythonContext() {
        Logger::instance().flushAll();

        assert(python::holdsGIL()); // make sure this thread holds the GIL!
        python::unlockGIL();

        if(_context)
            delete _context;
        // need to hold GIL,
        // i.e. restore GIL
        python::lockGIL();
        _context = nullptr;
    }


    boost::python::dict PythonContext::options() const {
        assert(_context);
        ContextOptions co = _context->getOptions();

        assert(PyGILState_Check()); // make sure this thread holds the GIL!
        PyObject* dictObject = PyDict_New();

        // bool options
        PyDict_SetItem(dictObject,
                python::PyString_FromString("tuplex.useLLVMOptimizer"),
                python::boolToPython(co.USE_LLVM_OPTIMIZER()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.autoUpcast"),
                       python::boolToPython(co.AUTO_UPCAST_NUMBERS()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.allowUndefinedBehavior"),
                       python::boolToPython(co.UNDEFINED_BEHAVIOR_FOR_OPERATORS()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.optimizer.codeStats"),
                       python::boolToPython(co.OPT_DETAILED_CODE_STATS()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.optimizer.generateParser"),
                       python::boolToPython(co.OPT_GENERATE_PARSER()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.optimizer.nullValueOptimization"),
                       python::boolToPython(co.OPT_NULLVALUE_OPTIMIZATION()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.optimizer.filterPushdown"),
                       python::boolToPython(co.OPT_FILTER_PUSHDOWN()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.optimizer.sharedObjectPropagation"),
                       python::boolToPython(co.OPT_SHARED_OBJECT_PROPAGATION()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.interleaveIO"),
                       python::boolToPython(co.INTERLEAVE_IO()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.resolveWithInterpreterOnly"),
                       python::boolToPython(co.RESOLVE_WITH_INTERPRETER_ONLY()));

        // @TODO: move to optimizer
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.csv.selectionPushdown"),
                       python::boolToPython(co.CSV_PARSER_SELECTION_PUSHDOWN()));


        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.webui.enable"),
                       python::boolToPython(co.USE_WEBUI()));

        // int options
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.executorCount"),
                       PyLong_FromLongLong(co.EXECUTOR_COUNT()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.csv.maxDetectionRows"),
                       PyLong_FromLongLong(co.CSV_MAX_DETECTION_ROWS()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.webui.port"),
                       PyLong_FromLongLong(co.WEBUI_PORT()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.webui.mongodb.port"),
                       PyLong_FromLongLong(co.WEBUI_DATABASE_PORT()));
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.webui.exceptionDisplayLimit"),
                       PyLong_FromLongLong(co.WEBUI_EXCEPTION_DISPLAY_LIMIT()));

        // aws options
        //@TODO:


        // float options
        PyDict_SetItem(dictObject,
                       python::PyString_FromString("tuplex.normalcaseThreshold"),
                       PyFloat_FromDouble(co.NORMALCASE_THRESHOLD()));

        // boost python has problems with the code below. I.e. somehow the nested structure does not
        // get correctly copied. Hence, there is a hack for these two in options() in Context.py
        // // list options
        // PyObject* list = nullptr;
        // auto vSeparators = co.CSV_SEPARATORS();
        // list = PyList_New(vSeparators.size());
        // for(unsigned i = 0; i < vSeparators.size(); ++i) {
        //     //PyList_SET_ITEM(list, i, python::PyString_FromChar(vSeparators[i]));
        //     PyList_SetItem(list, i, python::PyString_FromString(","));
        // }
        // PyDict_SetItem(dictObject,
        //                python::PyString_FromString("tuplex.csv.separators"),
        //                list);
        // auto vComments = co.CSV_COMMENTS();
        // list = PyList_New(vComments.size());
        // for(unsigned i = 0; i < vComments.size(); ++i) {
        //     PyList_SET_ITEM(list, i, python::PyString_FromChar(vComments[i]));
        // }
        // PyDict_SetItem(dictObject,
        //                python::PyString_FromString("tuplex.csv.comments"),
        //                list);

        // strings
        // i.e. for the rest
        auto store = co.store();
        for(auto keyval : store) {
            // check if contained in dict, if not add
            auto key = keyval.first;
            auto val = keyval.second;

            auto pykey = python::PyString_FromString(key.c_str());
            // if not contains, add
            // cf. https://docs.python.org/3/c-api/dict.html
            if(PyDict_Contains(dictObject, pykey) == 0) {
                PyObject* pyval = python::PyString_FromString(val.c_str());
                PyDict_SetItem(dictObject, pykey, pyval);
            }
        }

        // this is a backup function, to set all remaining options as strings, so nothing gets lost...
        for(auto keyval : co.store()) {
            // check if exists in dict
            if(!PyDict_Contains(dictObject, python::PyString_FromString(keyval.first.c_str()))) {
                auto py_key = python::PyString_FromString(keyval.first.c_str());
                auto py_val = python::PyString_FromString(keyval.second.c_str());
                PyDict_SetItem(dictObject, py_key, py_val);
            }
        }

        // first manual fetch
       return boost::python::dict(boost::python::handle<>(dictObject));
    }

    boost::python::object PythonContext::ls(const std::string &pattern) const {
        Timer timer;
        python::unlockGIL();
        auto uris = VirtualFileSystem::globAll(pattern);
        python::lockGIL();

        // create list object from result
        auto listObj = PyList_New(uris.size());
        for(unsigned i = 0; i < uris.size(); ++i) {
            PyList_SET_ITEM(listObj, i, python::PyString_FromString(uris[i].toPath().c_str()));
        }
        Logger::instance().logger("filesystem").info("listed " + std::to_string(uris.size()) + " files in " + std::to_string(timer.time()) +"s");
        Logger::instance().flushAll();
        return boost::python::list(boost::python::handle<>(listObj));
    }

    void PythonContext::cp(const std::string &pattern, const std::string &target) const {
        throw std::runtime_error("not yet supported");
    }

    void PythonContext::rm(const std::string &pattern) const {
        Timer timer;
        python::unlockGIL();
        auto rc = VirtualFileSystem::remove(pattern);
        python::lockGIL();
        if(rc != VirtualFileSystemStatus::VFS_OK)
            Logger::instance().logger("filesystem").error("failed to remove files from " + pattern);
        Logger::instance().logger("filesystem").info("removed files in " + std::to_string(timer.time()) +"s");
        Logger::instance().flushAll();
    }
}