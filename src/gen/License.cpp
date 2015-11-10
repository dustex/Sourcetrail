#include "License.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <istream>

// #include "botan/pubkey.h"
// #include "botan/base64.h"
// #include "botan/passhash9.h"

#include "boost/filesystem.hpp"

// #ifdef BOTAN_HAS_RSA
// #include "botan/rsa.h"
// #endif

License::License()
{
}

License::~License()
{
}

std::string License::getHashLine()
{
    return lines[4];
}

std::string License::getMessage()
{
    std::string message = "";
    for(int i = 1; i < 5; ++i)
    {
        message += lines[i];
    }
    return message;
}

std::string License::getSignature()
{
    std::string signatue = "";
    for(int i = 5; i < 12; ++i)
    {
        signatue += lines[i];
    }
    return signatue;
}

std::string License::getVersionLine()
{
    return lines[3];
}

void License::create(std::string user, std::string version, Botan::RSA_PrivateKey* privateKey, unsigned int type)
{
    m_version = version;
    createMessage(user, version, type);

    //encode message
    Botan::PK_Signer signer(*privateKey, "EMSA4(SHA-256)");
    Botan::DataSource_Memory in(getMessage());
    Botan::byte buf[4096] = {0};
    while (size_t got = in.read(buf, sizeof(buf))) {
        signer.update(buf, got);
    }
    std::string signature = Botan::base64_encode(signer.signature(m_rng));
    addSignature(signature);
}

void License::createMessage(std::string user, std::string version, unsigned int type)
{
    lines.clear();
    lines.push_back(BEGIN_LICENSE);
    lines.push_back(user);
    std::string typestring;
    switch(type)
    {
        case 0:
        default:
            typestring = "Single User License";

    };
    lines.push_back(typestring);
    std::string versionstring = "Coati " + getVersion();
    lines.push_back(versionstring);
    std::string pass9 = Botan::generate_passhash9(versionstring, m_rng);
    lines.push_back(pass9);
}

void License::writeToFile(std::string filename)
{
    std::ofstream licenseFile(filename);
    for(std::string line : lines)
    {
        licenseFile << line << std::endl;
    }
}

bool License::loadFromString(std::string licenseText)
{
    lines.clear();
    std::istringstream license(licenseText);
    return load(license);
}

bool License::load(std::istream& stream)
{
    lines.clear();
    std::string line;
    if(!getline(stream, line, '\n')) 
    {
        return false;
    }
    if(line.compare(BEGIN_LICENSE) )
    {
        std::cout << "No License Header" << std::endl;
        lines.push_back(BEGIN_LICENSE);        
    }
    else
    {        
        lines.push_back(line);
        if(!getline(stream, line))
        {
            return false;
        }
    }
    lines.push_back(line);

    for(int i = 0; i < 2; ++i)
    {
        if(!getline(stream, line))
        {
            return false;
        }
        lines.push_back(line);
    }
    if(!getline(stream, line))
    {
        return false;
    }
    lines.push_back(line);

    //get signature
    std::string signature = "";
    for(int i = 0; i < 7; ++i)
    {
        if(!getline(stream, line))
        {
            return false;
        }
        signature += line;
        lines.push_back(line);
    }

    //check License ending
    getline(stream, line);
    if(line.compare(END_LICENSE))
    {
        std::cout << "No License Footer" << std::endl;
        lines.push_back(END_LICENSE);        
    }
    else
    {
        lines.push_back(line);
    }
    return true;
}


bool License::loadFromFile(std::string filename)
{
    lines.clear();
    std::ifstream sigfile(filename);
    return load(sigfile);
}

void License::print()
{
    std::cout << getLicenseString();
}

std::string License::getOwnerLine()
{
    return lines[1];
}

std::string License::getLicenseTypeLine()
{
    return lines[2];
}

void License::addSignature(std::string signature)
{
    if(lines.size() > 5)
    {
        std::cout << "signature already there" << std::endl;
        return;
    }
    if (!signature.size()) {
        std::cout << "signature is empty." << std::endl;
        return;
    }
    std::stringstream ss;
    for (size_t i = 0; i < signature.size(); i++) {
        if(i % 55 == 0 && i != 0)
        {
            lines.push_back(ss.str());
            ss.str("");
        }
        ss << signature[i];
    }
    lines.push_back(ss.str());
    lines.push_back(END_LICENSE);
}

bool License::isValid()
{
    if(!m_publicKey)
    {
        std::cout << "No public key loaded" << std::endl;
        return false;
    }
    if(Botan::check_passhash9("Coati "+ getVersion(), getHashLine()))
    {
        std::cout << "Hash from Coati "+ getVersion() + " confirmed" << std::endl;
    }

    Botan::secure_vector<Botan::byte> sig = Botan::base64_decode(getSignature());

    Botan::PK_Verifier verifier(*m_publicKey.get(), "EMSA4(SHA-256)");

    Botan::DataSource_Memory in(getMessage());
    Botan::byte buf[4096] = {0};
    while(size_t got = in.read(buf, sizeof(buf)))
    {
        verifier.update(buf, got);
    }

    const bool ok = verifier.check_signature(sig);
    return ok;
}

std::string License::getPublicKeyFilename()
{
    if(m_publicKeyFilename.empty())
    {
        return "public-" + getVersion() + KEY_FILEENDING;
    }
    return m_publicKeyFilename;
}

std::string License::getVersion() {
    if(m_version.empty())
    {
        return "x";
    }
    return m_version;
}

bool License::loadPublicKeyFromFile(std::string filename)
{
    if (!filename.empty()) {
        m_publicKeyFilename = filename;
    }
    if(boost::filesystem::exists(getPublicKeyFilename()))
    {
        Botan::RSA_PublicKey *rsaPublicKey = dynamic_cast<Botan::RSA_PublicKey *>(Botan::X509::load_key(getPublicKeyFilename()));
        if (!rsaPublicKey) {
            std::cout << "The loaded key is not a RSA key" << std::endl;
            return false;
        }
        m_publicKey = std::shared_ptr<Botan::RSA_PublicKey>(rsaPublicKey);
        return true;
    }
    return false;
}

bool License::loadPublicKeyFromString(std::string publicKey)
{
    Botan::DataSource_Memory in(publicKey);
    Botan::RSA_PublicKey *rsaPublicKey = dynamic_cast<Botan::RSA_PublicKey *>(Botan::X509::load_key(in));
    if (!rsaPublicKey) {
        std::cout << "The loaded key is not a RSA key" << std::endl;
        return false;
    }
    m_publicKey = std::shared_ptr<Botan::RSA_PublicKey>(rsaPublicKey);
    return true;
}


void License::setVersion(const std::string& version)
{
    if(!version.empty())
    {
        m_version = version;
    }
}

std::string License::getLicenseString()
{
    std::stringstream license;
    for(std::string line : lines)
    {
        license << line << std::endl;
    }
    return license.str();
}

bool License::checkLocation(const std::string& location, const std::string& hash)
{
    return Botan::check_passhash9(location, hash);
}

std::string License::hashLocation(const std::string& location)
{
    return Botan::generate_passhash9(location, m_rng);
}
