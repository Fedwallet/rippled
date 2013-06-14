DECLARE_INSTANCE (SHAMapTreeNode);

SHAMapTreeNode::SHAMapTreeNode (uint32 seq, const SHAMapNode& nodeID) : SHAMapNode (nodeID), mHash (0),
    mSeq (seq), mAccessSeq (seq), mType (tnERROR), mIsBranch (0), mFullBelow (false)
{
}

SHAMapTreeNode::SHAMapTreeNode (const SHAMapTreeNode& node, uint32 seq) : SHAMapNode (node),
    mHash (node.mHash), mSeq (seq), mType (node.mType), mIsBranch (node.mIsBranch), mFullBelow (false)
{
    if (node.mItem)
        mItem = boost::make_shared<SHAMapItem> (*node.mItem);
    else
        memcpy (mHashes, node.mHashes, sizeof (mHashes));
}

SHAMapTreeNode::SHAMapTreeNode (const SHAMapNode& node, SHAMapItem::ref item, TNType type, uint32 seq) :
    SHAMapNode (node), mItem (item), mSeq (seq), mType (type), mIsBranch (0), mFullBelow (true)
{
    assert (item->peekData ().size () >= 12);
    updateHash ();
}

SHAMapTreeNode::SHAMapTreeNode (const SHAMapNode& id, Blob const& rawNode, uint32 seq,
                                SHANodeFormat format, uint256 const& hash, bool hashValid) :
    SHAMapNode (id), mSeq (seq), mType (tnERROR), mIsBranch (0), mFullBelow (false)
{
    if (format == snfWIRE)
    {
        Serializer s (rawNode);
        int type = s.removeLastByte ();
        int len = s.getLength ();

        if ((type < 0) || (type > 4))
        {
#ifdef DEBUG
            std::cerr << "Invalid wire format node" << std::endl;
            std::cerr << strHex (rawNode) << std::endl;
            assert (false);
#endif
            throw std::runtime_error ("invalid node AW type");
        }

        if (type == 0)
        {
            // transaction
            mItem = boost::make_shared<SHAMapItem> (s.getPrefixHash (sHP_TransactionID), s.peekData ());
            mType = tnTRANSACTION_NM;
        }
        else if (type == 1)
        {
            // account state
            if (len < (256 / 8))
                throw std::runtime_error ("short AS node");

            uint256 u;
            s.get256 (u, len - (256 / 8));
            s.chop (256 / 8);

            if (u.isZero ()) throw std::runtime_error ("invalid AS node");

            mItem = boost::make_shared<SHAMapItem> (u, s.peekData ());
            mType = tnACCOUNT_STATE;
        }
        else if (type == 2)
        {
            // full inner
            if (len != 512)
                throw std::runtime_error ("invalid FI node");

            for (int i = 0; i < 16; ++i)
            {
                s.get256 (mHashes[i], i * 32);

                if (mHashes[i].isNonZero ())
                    mIsBranch |= (1 << i);
            }

            mType = tnINNER;
        }
        else if (type == 3)
        {
            // compressed inner
            for (int i = 0; i < (len / 33); ++i)
            {
                int pos;
                s.get8 (pos, 32 + (i * 33));

                if ((pos < 0) || (pos >= 16)) throw std::runtime_error ("invalid CI node");

                s.get256 (mHashes[pos], i * 33);

                if (mHashes[pos].isNonZero ())
                    mIsBranch |= (1 << pos);
            }

            mType = tnINNER;
        }
        else if (type == 4)
        {
            // transaction with metadata
            if (len < (256 / 8))
                throw std::runtime_error ("short TM node");

            uint256 u;
            s.get256 (u, len - (256 / 8));
            s.chop (256 / 8);

            if (u.isZero ())
                throw std::runtime_error ("invalid TM node");

            mItem = boost::make_shared<SHAMapItem> (u, s.peekData ());
            mType = tnTRANSACTION_MD;
        }
    }

    else if (format == snfPREFIX)
    {
        if (rawNode.size () < 4)
        {
            WriteLog (lsINFO, SHAMapNode) << "size < 4";
            throw std::runtime_error ("invalid P node");
        }

        uint32 prefix = rawNode[0];
        prefix <<= 8;
        prefix |= rawNode[1];
        prefix <<= 8;
        prefix |= rawNode[2];
        prefix <<= 8;
        prefix |= rawNode[3];
        Serializer s (rawNode.begin () + 4, rawNode.end ());

        if (prefix == sHP_TransactionID)
        {
            mItem = boost::make_shared<SHAMapItem> (Serializer::getSHA512Half (rawNode), s.peekData ());
            mType = tnTRANSACTION_NM;
        }
        else if (prefix == sHP_LeafNode)
        {
            if (s.getLength () < 32)
                throw std::runtime_error ("short PLN node");

            uint256 u;
            s.get256 (u, s.getLength () - 32);
            s.chop (32);

            if (u.isZero ())
            {
                WriteLog (lsINFO, SHAMapNode) << "invalid PLN node";
                throw std::runtime_error ("invalid PLN node");
            }

            mItem = boost::make_shared<SHAMapItem> (u, s.peekData ());
            mType = tnACCOUNT_STATE;
        }
        else if (prefix == sHP_InnerNode)
        {
            if (s.getLength () != 512)
                throw std::runtime_error ("invalid PIN node");

            for (int i = 0; i < 16; ++i)
            {
                s.get256 (mHashes[i], i * 32);

                if (mHashes[i].isNonZero ())
                    mIsBranch |= (1 << i);
            }

            mType = tnINNER;
        }
        else if (prefix == sHP_TransactionNode)
        {
            // transaction with metadata
            if (s.getLength () < 32)
                throw std::runtime_error ("short TXN node");

            uint256 txID;
            s.get256 (txID, s.getLength () - 32);
            s.chop (32);
            mItem = boost::make_shared<SHAMapItem> (txID, s.peekData ());
            mType = tnTRANSACTION_MD;
        }
        else
        {
            WriteLog (lsINFO, SHAMapNode) << "Unknown node prefix " << std::hex << prefix << std::dec;
            throw std::runtime_error ("invalid node prefix");
        }
    }

    else
    {
        assert (false);
        throw std::runtime_error ("Unknown format");
    }

    if (hashValid)
    {
        mHash = hash;
#ifdef PARANOID
        updateHash ();
        assert (mHash == hash);
#endif
    }
    else
        updateHash ();
}

bool SHAMapTreeNode::updateHash ()
{
    uint256 nh;

    if (mType == tnINNER)
    {
        if (mIsBranch != 0)
        {
            nh = Serializer::getPrefixHash (sHP_InnerNode, reinterpret_cast<unsigned char*> (mHashes), sizeof (mHashes));
#ifdef PARANOID
            Serializer s;
            s.add32 (sHP_InnerNode);

            for (int i = 0; i < 16; ++i)
                s.add256 (mHashes[i]);

            assert (nh == s.getSHA512Half ());
#endif
        }
        else
            nh.zero ();
    }
    else if (mType == tnTRANSACTION_NM)
    {
        nh = Serializer::getPrefixHash (sHP_TransactionID, mItem->peekData ());
    }
    else if (mType == tnACCOUNT_STATE)
    {
        Serializer s (mItem->peekSerializer ().getDataLength () + (256 + 32) / 8);
        s.add32 (sHP_LeafNode);
        s.addRaw (mItem->peekData ());
        s.add256 (mItem->getTag ());
        nh = s.getSHA512Half ();
    }
    else if (mType == tnTRANSACTION_MD)
    {
        Serializer s (mItem->peekSerializer ().getDataLength () + (256 + 32) / 8);
        s.add32 (sHP_TransactionNode);
        s.addRaw (mItem->peekData ());
        s.add256 (mItem->getTag ());
        nh = s.getSHA512Half ();
    }
    else
        assert (false);

    if (nh == mHash)
        return false;

    mHash = nh;
    return true;
}

void SHAMapTreeNode::addRaw (Serializer& s, SHANodeFormat format)
{
    assert ((format == snfPREFIX) || (format == snfWIRE) || (format == snfHASH));

    if (mType == tnERROR)
        throw std::runtime_error ("invalid I node type");

    if (format == snfHASH)
    {
        s.add256 (getNodeHash ());
    }
    else if (mType == tnINNER)
    {
        assert (!isEmpty ());

        if (format == snfPREFIX)
        {
            s.add32 (sHP_InnerNode);

            for (int i = 0; i < 16; ++i)
                s.add256 (mHashes[i]);
        }
        else
        {
            if (getBranchCount () < 12)
            {
                // compressed node
                for (int i = 0; i < 16; ++i)
                    if (!isEmptyBranch (i))
                    {
                        s.add256 (mHashes[i]);
                        s.add8 (i);
                    }

                s.add8 (3);
            }
            else
            {
                for (int i = 0; i < 16; ++i)
                    s.add256 (mHashes[i]);

                s.add8 (2);
            }
        }
    }
    else if (mType == tnACCOUNT_STATE)
    {
        if (format == snfPREFIX)
        {
            s.add32 (sHP_LeafNode);
            s.addRaw (mItem->peekData ());
            s.add256 (mItem->getTag ());
        }
        else
        {
            s.addRaw (mItem->peekData ());
            s.add256 (mItem->getTag ());
            s.add8 (1);
        }
    }
    else if (mType == tnTRANSACTION_NM)
    {
        if (format == snfPREFIX)
        {
            s.add32 (sHP_TransactionID);
            s.addRaw (mItem->peekData ());
        }
        else
        {
            s.addRaw (mItem->peekData ());
            s.add8 (0);
        }
    }
    else if (mType == tnTRANSACTION_MD)
    {
        if (format == snfPREFIX)
        {
            s.add32 (sHP_TransactionNode);
            s.addRaw (mItem->peekData ());
            s.add256 (mItem->getTag ());
        }
        else
        {
            s.addRaw (mItem->peekData ());
            s.add256 (mItem->getTag ());
            s.add8 (4);
        }
    }
    else
        assert (false);
}

bool SHAMapTreeNode::setItem (SHAMapItem::ref i, TNType type)
{
    uint256 hash = getNodeHash ();
    mType = type;
    mItem = i;
    assert (isLeaf ());
    updateHash ();
    return getNodeHash () != hash;
}

SHAMapItem::pointer SHAMapTreeNode::getItem () const
{
    assert (isLeaf ());
    return boost::make_shared<SHAMapItem> (*mItem);
}

bool SHAMapTreeNode::isEmpty () const
{
    return mIsBranch == 0;
}

int SHAMapTreeNode::getBranchCount () const
{
    assert (isInner ());
    int count = 0;

    for (int i = 0; i < 16; ++i)
        if (!isEmptyBranch (i))
            ++count;

    return count;
}

void SHAMapTreeNode::makeInner ()
{
    mItem.reset ();
    mIsBranch = 0;
    memset (mHashes, 0, sizeof (mHashes));
    mType = tnINNER;
    mHash.zero ();
}

void SHAMapTreeNode::dump ()
{
    WriteLog (lsDEBUG, SHAMapNode) << "SHAMapTreeNode(" << getNodeID () << ")";
}

std::string SHAMapTreeNode::getString () const
{
    std::string ret = "NodeID(";
    ret += boost::lexical_cast<std::string> (getDepth ());
    ret += ",";
    ret += getNodeID ().GetHex ();
    ret += ")";

    if (isInner ())
    {
        for (int i = 0; i < 16; ++i)
            if (!isEmptyBranch (i))
            {
                ret += "\nb";
                ret += boost::lexical_cast<std::string> (i);
                ret += " = ";
                ret += mHashes[i].GetHex ();
            }
    }

    if (isLeaf ())
    {
        if (mType == tnTRANSACTION_NM)
            ret += ",txn\n";
        else if (mType == tnTRANSACTION_MD)
            ret += ",txn+md\n";
        else if (mType == tnACCOUNT_STATE)
            ret += ",as\n";
        else
            ret += ",leaf\n";

        ret += "  Tag=";
        ret += getTag ().GetHex ();
        ret += "\n  Hash=";
        ret += mHash.GetHex ();
        ret += "/";
        ret += lexical_cast_i (mItem->peekSerializer ().getDataLength ());
    }

    return ret;
}

bool SHAMapTreeNode::setChildHash (int m, uint256 const& hash)
{
    assert ((m >= 0) && (m < 16));
    assert (mType == tnINNER);

    if (mHashes[m] == hash)
        return false;

    mHashes[m] = hash;

    if (hash.isNonZero ())
        mIsBranch |= (1 << m);
    else
        mIsBranch &= ~ (1 << m);

    return updateHash ();
}
